#include <std_include.hpp>
#include "loader/component_loader.hpp"
#include "scheduler.hpp"

#include "steam/steam.hpp"
#include <utils/hook.hpp>

namespace arxan
{
	namespace
	{
		DWORD get_steam_pid()
		{
			HKEY reg_key;
			DWORD pid{};

			if (RegOpenKeyExA(HKEY_CURRENT_USER, "Software\\Valve\\Steam\\ActiveProcess", 0, KEY_QUERY_VALUE,
			                  &reg_key) != ERROR_SUCCESS)
				return pid;

			DWORD length = sizeof(pid);
			RegQueryValueExA(reg_key, "pid", nullptr, nullptr, reinterpret_cast<BYTE*>(&pid), &length);
			RegCloseKey(reg_key);

			return pid;
		}

		bool is_valid_pid(const DWORD pid)
		{
			const auto handle = OpenProcess(SYNCHRONIZE, FALSE, pid);
			if (!handle)
			{
				return false;
			}

			CloseHandle(handle);
			return true;
		}

		void start_steam()
		{
			STARTUPINFOA startup_info;
			PROCESS_INFORMATION process_info;

			ZeroMemory(&startup_info, sizeof(startup_info));
			ZeroMemory(&process_info, sizeof(process_info));
			startup_info.cb = sizeof(startup_info);

			const auto steam_folder = steam::SteamAPI_GetSteamInstallPath();
			const auto steam_path = steam_folder + "\\steam.exe"s;
			char cmd_line[] = "steam.exe -silent";

			if (CreateProcessA(steam_path.data(), &cmd_line[0], nullptr, nullptr, false, NULL, nullptr, steam_folder,
			                   &startup_info, &process_info))
			{
				CloseHandle(process_info.hThread);
				CloseHandle(
					process_info.hProcess);
			}
		}

		void start_steam_if_wanted()
		{
			if (MessageBoxA(nullptr, "Steam must be running. Do you want to start it now?", "Information",
			                MB_ICONINFORMATION | MB_YESNO) != IDYES)
			{
				TerminateProcess(GetCurrentProcess(), 1);
			}

			start_steam();
		}

		DWORD setup_and_get_steam_pid()
		{
			static DWORD steam_pid = []
			{
				auto pid = get_steam_pid();
				if (is_valid_pid(pid))
				{
					return pid;
				}

				start_steam_if_wanted();

				std::this_thread::sleep_for(3s);

				for (int i = 0; i < 10; ++i)
				{
					std::this_thread::sleep_for(1s);
					pid = get_steam_pid();
					if (is_valid_pid(pid))
					{
						return pid;
					}
				}

				return get_steam_pid();
			}();

			return steam_pid;
		}

		utils::hook::detour nt_close_hook;
		utils::hook::detour nt_query_system_information_hook;
		utils::hook::detour nt_query_information_process_hook;
		utils::hook::detour create_mutex_ex_a_hook;

		HANDLE create_mutex_ex_a_stub(const LPSECURITY_ATTRIBUTES attributes, const LPCSTR name, const DWORD flags,
		                              const DWORD access)
		{
			if (name == "$ IDA trusted_idbs"s || name == "$ IDA registry mutex $"s)
			{
				return nullptr;
			}

			return create_mutex_ex_a_hook.invoke<HANDLE>(attributes, name, flags, access);
		}

		bool remove_evil_keywords_from_string(const UNICODE_STRING& string)
		{
			static const std::wstring evil_keywords[] =
			{
				L"IDA",
				L"ida",
				L"HxD",
				L"cheatengine",
				L"Cheat Engine",
			};

			if (!string.Buffer || !string.Length)
			{
				return false;
			}

			std::wstring_view path(string.Buffer, string.Length / sizeof(string.Buffer[0]));

			bool modified = false;
			for (const auto& keyword : evil_keywords)
			{
				while (true)
				{
					const auto pos = path.find(keyword);
					if (pos == std::wstring::npos)
					{
						break;
					}

					modified = true;

					for (size_t i = 0; i < keyword.size(); ++i)
					{
						string.Buffer[pos + i] = L'a';
					}
				}
			}

			return modified;
		}

		bool remove_evil_keywords_from_string(wchar_t* str, const size_t length)
		{
			UNICODE_STRING unicode_string{};
			unicode_string.Buffer = str;
			unicode_string.Length = static_cast<uint16_t>(length);
			unicode_string.MaximumLength = unicode_string.Length;

			return remove_evil_keywords_from_string(unicode_string);
		}

		bool remove_evil_keywords_from_string(char* str, const size_t length)
		{
			std::string_view str_view(str, length);
			std::wstring wstr(str_view.begin(), str_view.end());

			if (!remove_evil_keywords_from_string(&wstr[0], wstr.size()))
			{
				return false;
			}

			const std::string regular_str(wstr.begin(), wstr.end());
			memcpy(str, regular_str.data(), length);

			return true;
		}


		int WINAPI get_window_text_a_stub(const HWND wnd, const LPSTR str, const int max_count)
		{
			std::wstring wstr{};
			wstr.resize(max_count);

			const auto res = GetWindowTextW(wnd, &wstr[0], max_count);
			if (res)
			{
				remove_evil_keywords_from_string(wstr.data(), res);

				const std::string regular_str(wstr.begin(), wstr.end());
				memset(str, 0, max_count);
				memcpy(str, regular_str.data(), res);
			}

			return res;
		}

		NTSTATUS NTAPI nt_query_system_information_stub(const SYSTEM_INFORMATION_CLASS system_information_class,
		                                                const PVOID system_information,
		                                                const ULONG system_information_length,
		                                                const PULONG return_length)
		{
			const auto status = nt_query_system_information_hook.invoke<NTSTATUS>(
				system_information_class, system_information, system_information_length, return_length);

			if (NT_SUCCESS(status))
			{
				if (system_information_class == SystemProcessInformation)
				{
					auto addr = static_cast<uint8_t*>(system_information);
					while (true)
					{
						const auto info = reinterpret_cast<SYSTEM_PROCESS_INFORMATION*>(addr);
						remove_evil_keywords_from_string(info->ImageName);

						if (!info->NextEntryOffset)
						{
							break;
						}

						addr = addr + info->NextEntryOffset;
					}
				}
			}

			return status;
		}

#define ProcessDebugPort 7
#define ProcessDebugObjectHandle 30 // WinXP source says 31?
#define ProcessDebugFlags 31 // WinXP source says 32?

		NTSTATUS WINAPI nt_query_information_process_stub(HANDLE handle, const PROCESSINFOCLASS info_class,
		                                                  const PVOID info,
		                                                  const ULONG info_length, const PULONG ret_length)
		{
			auto* orig = static_cast<decltype(NtQueryInformationProcess)*>(nt_query_information_process_hook.
				get_original());

			const auto status = orig(handle, info_class, info, info_length, ret_length);

			if (NT_SUCCESS(status))
			{
				if (info_class == ProcessBasicInformation)
				{
					static_cast<PPROCESS_BASIC_INFORMATION>(info)->Reserved3 =
						PVOID(DWORD64(setup_and_get_steam_pid()));
				}
				else if (info_class == ProcessDebugObjectHandle)
				{
					*static_cast<HANDLE*>(info) = nullptr;

					return 0xC0000353;
				}
				else if (info_class == ProcessImageFileName || info_class == 43 /* ? */)
				{
					remove_evil_keywords_from_string(*static_cast<UNICODE_STRING*>(info));
				}
				else if (info_class == ProcessDebugPort)
				{
					*static_cast<HANDLE*>(info) = nullptr;
				}
				else if (info_class == ProcessDebugFlags)
				{
					*static_cast<ULONG*>(info) = 1;
				}
			}

			return status;
		}

		NTSTATUS NTAPI nt_close_stub(const HANDLE handle)
		{
			char info[16];
			if (NtQueryObject(handle, OBJECT_INFORMATION_CLASS(4), &info, 2, nullptr) >= 0 && size_t(handle) != 0x12345)
			{
				auto* orig = static_cast<decltype(NtClose)*>(nt_close_hook.get_original());
				return orig(handle);
			}

			return STATUS_INVALID_HANDLE;
		}

		LONG WINAPI exception_filter(const LPEXCEPTION_POINTERS info)
		{
			if (info->ExceptionRecord->ExceptionCode == STATUS_INVALID_HANDLE)
			{
				return EXCEPTION_CONTINUE_EXECUTION;
			}

			if (info->ExceptionRecord->ExceptionCode == STATUS_ACCESS_VIOLATION)
			{
				//MessageBoxA(0, 0, "AV", 0);
			}

			return EXCEPTION_CONTINUE_SEARCH;
		}

		void hide_being_debugged()
		{
			auto* const peb = PPEB(__readgsqword(0x60));
			peb->BeingDebugged = false;
			*reinterpret_cast<PDWORD>(LPSTR(peb) + 0xBC) &= ~0x70;
		}

		void restore_debug_functions()
		{
			static const char* functions[] = {
				"DbgBreakPoint",
				"DbgUserBreakPoint",
				"DbgUiConnectToDbg",
				"DbgUiContinue",
				"DbgUiConvertStateChangeStructure",
				"DbgUiDebugActiveProcess",
				"DbgUiGetThreadDebugObject",
				"DbgUiIssueRemoteBreakin",
				"DbgUiRemoteBreakin",
				"DbgUiSetThreadDebugObject",
				"DbgUiStopDebugging",
				"DbgUiWaitStateChange",
				"DbgPrintReturnControlC",
				"DbgPrompt",
			};

			using buffer = uint8_t[15];
			static buffer buffers[ARRAYSIZE(functions)] = {};
			static bool loaded = false;

			const utils::nt::library ntdll("ntdll.dll");

			for (auto i = 0u; i < ARRAYSIZE(functions); ++i)
			{
				const auto func = ntdll.get_proc<void*>(functions[i]);
				if (!loaded)
				{
					memcpy(buffers[i], func, sizeof(buffer));
				}
				else
				{
					utils::hook::copy(func, buffers[i], sizeof(buffer));
				}
			}

			loaded = true;
		}
	}

	class component final : public component_interface
	{
	public:
		void post_load() override
		{
			setup_and_get_steam_pid();

			hide_being_debugged();
			scheduler::loop(hide_being_debugged, scheduler::pipeline::async);

			//restore_debug_functions();

			create_mutex_ex_a_hook.create(CreateMutexExA, create_mutex_ex_a_stub);


			const utils::nt::library ntdll("ntdll.dll");
			nt_close_hook.create(ntdll.get_proc<void*>("NtClose"), nt_close_stub);

			const auto nt_query_information_process = ntdll.get_proc<void*>("NtQueryInformationProcess");
			nt_query_information_process_hook.create(nt_query_information_process,
			                                         nt_query_information_process_stub);

			const auto nt_query_system_information = ntdll.get_proc<void*>("NtQuerySystemInformation");
			nt_query_system_information_hook.create(nt_query_system_information, nt_query_system_information_stub);
			nt_query_system_information_hook.move();

			utils::hook::copy(this->window_text_buffer_, GetWindowTextA, sizeof(this->window_text_buffer_));
			utils::hook::jump(GetWindowTextA, get_window_text_a_stub, true, true);
			utils::hook::move_hook(GetWindowTextA);

			AddVectoredExceptionHandler(1, exception_filter);
		}

		void pre_destroy() override
		{
			utils::hook::copy(GetWindowTextA, this->window_text_buffer_, sizeof(this->window_text_buffer_));
			nt_query_system_information_hook.clear();
			nt_query_information_process_hook.clear();
			nt_close_hook.clear();
			create_mutex_ex_a_hook.clear();
		}

	private:
		uint8_t window_text_buffer_[15]{};
	};
}

REGISTER_COMPONENT(arxan::component)
