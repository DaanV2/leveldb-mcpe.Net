// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#if defined(_MSC_VER)

#define VC_EXTRALEAN            // Exclude rarely-used stuff
#define WIN32_LEAN_AND_MEAN     // Exclude rarely-used stuff from Windows headers
#include "Filepath.h"
#include "include/leveldb/env.h"
#include "include/leveldb/slice.h"
#include "leveldb/env.h"
#include "leveldb/slice.h"
#include "port/port.h"
#include "util/logging.h"
#include "util/win_logger.h"
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <ctime>
#include <deque>
#include <errno.h>
#include <fcntl.h>
#include <fstream>
#include <io.h>
#include <memory>
#include <sstream>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <thread>
#include <time.h>
#include <windows.h>

#define MAX_FILENAME 512

namespace LevelDB {
	namespace {

		// returns the ID of the current process
		static uint32_t current_process_id(void) {
			return static_cast<uint32_t>(::GetCurrentProcessId());
		}

		ref class NoOpLogger : public Logger {
		public:
			virtual void Logv(const char* format, va_list ap) { }
		};

		ref struct IOException : public std::exception
		{
			System::String s;
			IOException(System::String ss) : s(ss) {}
			~IOException() throw () {} // Updated
			const char* what() const throw() { return s.c_str(); }
		};

		static System::String ws2s(const std::wstring& ws)
		{
			int len;
			int wslength = (int)ws.length() + 1;
			len = WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), wslength, 0, 0, NULL, NULL);
			char* buf = new char[len];
			WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), wslength, buf, len, NULL, NULL);
			System::String r(buf);
			delete[] buf;
			return r;
		}

		static Status GetLastWindowsError(const System::String& name) {
			WCHAR lpBuffer[256] = L"?";
			FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM,                 // It's a system error
				NULL,                                      // No string to be formatted needed
				GetLastError(),                               // Hey Windows: Please explain this error!
				MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),  // Do it in the standard language
				lpBuffer,              // Put the message here
				(sizeof(lpBuffer) / sizeof(WCHAR)),         // Number of characters to store the message
				NULL);
			return Status::IOError(name, ws2s(lpBuffer).c_str());
		}

		static std::wstring GetFullPath(const System::String& fname) {
			return ::Port::toFilePath(fname);
		}

		static void EnsureDirectory(const System::String& fname)
		{
			System::String dir = fname;
			std::replace(dir.begin(), dir.end(), '/', '\\');
			char tmpName[MAX_FILENAME];
			strcpy_s(tmpName, dir.c_str());

			// Create parent directories
			for (char* p = strchr(tmpName, '\\'); p; p = strchr(p + 1, '\\')) {
				*p = 0;
				::CreateDirectoryW(GetFullPath(tmpName).c_str(), NULL);  // may or may not already exist
				*p = '\\';
			}
		}

		static Status OpenFile(const System::String& fname, DWORD dwDesiredAccess, DWORD dwShareMode, DWORD dwCreationDisposition, HANDLE& file, DWORD dwFlags = 0)
		{
			EnsureDirectory(fname);
			std::wstring path = GetFullPath(fname);
#if WINAPI_FAMILY_PARTITION(WINAPI_PARTITION_APP | WINAPI_PARTITION_SYSTEM) && (_WIN32_WINNT >= 0x0602)
			CREATEFILE2_EXTENDED_PARAMETERS extraParams;
			ZeroMemory(&extraParams, sizeof(extraParams));
			extraParams.dwFileAttributes = FILE_ATTRIBUTE_NORMAL;
			extraParams.dwSize = sizeof(extraParams);
			extraParams.dwFileFlags = dwFlags;

			file = ::CreateFile2(path.c_str(),
				dwDesiredAccess,
				dwShareMode,
				dwCreationDisposition, 
				&extraParams);
#else
			file = ::CreateFileW(path.c_str(),
				dwDesiredAccess,
				dwShareMode,
				NULL, 
				dwCreationDisposition,
				dwFlags ? dwFlags : FILE_ATTRIBUTE_NORMAL, 
				NULL);
#endif
			return (file == INVALID_HANDLE_VALUE ? GetLastWindowsError(fname) : Status::OK());
		}

		static Status CloseFile(const System::String& fname, HANDLE& file)
		{
			if (file != INVALID_HANDLE_VALUE)
			{
				BOOL ret = ::CloseHandle(file);
				file = INVALID_HANDLE_VALUE;
				return (!ret ? GetLastWindowsError(fname) : Status::OK());
			}
			else
				return Status::OK();
		}

		ref class WinSequentialFile : public SequentialFile {
		private:
			System::String _fname;
			HANDLE _file;

		public:

			WinSequentialFile(const System::String& fname)
				: _fname(fname) 
			{
				Status s = OpenFile(fname, GENERIC_READ, FILE_SHARE_READ, OPEN_EXISTING, _file);
				if (!s.ok())
					throw IOException(s.ToString().c_str());
			}

			virtual ~WinSequentialFile() 
			{ 
				CloseFile(_fname, _file);
			}

			virtual Status Read(size_t n, Slice* result, char* scratch)
			{
				DWORD dwRead;
				BOOL ret = ::ReadFile(_file, scratch, n, &dwRead, NULL);
				if (!ret)
					return GetLastWindowsError(_fname);
				*result = Slice(scratch, dwRead);
				if (dwRead < n)
				{
					LARGE_INTEGER cur, end;
					ret = ::SetFilePointerEx(_file, LARGE_INTEGER(), &cur, FILE_CURRENT);
					if (!ret)
						return GetLastWindowsError(_fname);
					ret = ::SetFilePointerEx(_file, LARGE_INTEGER(), &end, FILE_END);
					if (!ret)
						return GetLastWindowsError(_fname);
					if (end.QuadPart > cur.QuadPart)
					{
						// couldn't read enough bytes
						::SetFilePointerEx(_file, cur, NULL, FILE_CURRENT);
						return Status::IOError(_fname, "Couldn't read all data");
					}
					else
						return Status::OK();
				}
				else
					return Status::OK();

			}

			virtual Status Skip(uint64_t n) {
				LARGE_INTEGER cur;
				cur.QuadPart = n;
				return (!::SetFilePointerEx(_file, cur, NULL, FILE_CURRENT) ? GetLastWindowsError(_fname) : Status::OK());
			}
		};

		ref class WinRandomAccessFile : public RandomAccessFile {
		private:
			System::String _fname;
			HANDLE _file;
		public:
			WinRandomAccessFile(const System::String& fname)
				: _fname(fname)
			{
				Status s = OpenFile(fname, GENERIC_READ, FILE_SHARE_READ, OPEN_EXISTING, _file, FILE_FLAG_OVERLAPPED | FILE_FLAG_RANDOM_ACCESS);
				if (!s.ok())
					throw IOException(s.ToString().c_str());
			}

			virtual ~WinRandomAccessFile() 
			{ 
				CloseFile(_fname, _file);
			}

			virtual Status Read(uint64_t offset, size_t n, Slice* result, char* scratch) const {
				OVERLAPPED readDesc;
				ZeroMemory(&readDesc, sizeof(readDesc));
				readDesc.Offset = offset;
				readDesc.OffsetHigh = offset >> 32;
				readDesc.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);

				if (readDesc.hEvent == NULL) {
					return GetLastWindowsError(_fname);
				}
				DWORD dwRead = 0;
				BOOL ret = ::ReadFile(_file, scratch, n, NULL, &readDesc);

				// the function might be completing asynchronously
				if (ret == 0 && GetLastError() != ERROR_IO_PENDING) {
					::CloseHandle(readDesc.hEvent);
					return GetLastWindowsError(_fname);
				}

				// Wait until the read is completed
				ret = WaitForSingleObject(readDesc.hEvent, INFINITE);
				if (ret == WAIT_FAILED) {
					::CloseHandle(readDesc.hEvent);
					return GetLastWindowsError(_fname);
				}

				// then read the result and the read bytes
				ret = GetOverlappedResult(_file, &readDesc, &dwRead, FALSE);
				
				if(ret == 0) {
					::CloseHandle(readDesc.hEvent);
					return GetLastWindowsError(_fname);
				}

				*result = Slice(scratch, dwRead);

				::CloseHandle(readDesc.hEvent);
				return Status::OK();
			}
		};

		// We preallocate up to an extra megabyte and use memcpy to append new
		// data to the file.  This is safe since we either properly close the
		// file before reading from it, or for log files, the reading code
		// knows enough to skip zero suffixes.

		ref class WinFile : public WritableFile {

		private:
			System::String _fname;
			HANDLE _file;

		public:
			explicit WinFile(System::String fname) : _fname(fname) {
				Status s = OpenFile(fname, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, CREATE_ALWAYS, _file);
				if (!s.ok())
					throw IOException(s.ToString().c_str());
			}

			virtual ~WinFile() {
				Close();
			}

		private:
		public:
			virtual Status Append(const Slice& data) {
				DWORD dwWritten;
				BOOL ret = ::WriteFile(_file, data.data(), data.size(), &dwWritten, NULL);
				return ((!ret || dwWritten < data.size()) ? GetLastWindowsError(_fname) : Status::OK());
			}

			virtual Status Close() {
				return CloseFile(_fname, _file);
			}

			virtual Status Flush() {
				//BOOL ret = ::FlushFileBuffers(_file);
				//return (!ret ? GetLastWindowsError(_fname) : Status::OK());
				return Status::OK();
			}
	
			virtual Status Sync() {
				BOOL ret = ::FlushFileBuffers(_file);
				return (!ret ? GetLastWindowsError(_fname) : Status::OK());
				//return Flush();
			}
		};

		ref class WinFileLock : public FileLock {
		private:
			System::String _fname;
			HANDLE _file;
			DWORD _fileSizeHigh;
			DWORD _fileSizeLow;
		public:
			WinFileLock(const System::String& fname) 
				: _fname(fname) 
			{
				FILE_STANDARD_INFO fi;
				Status s = OpenFile(fname, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, OPEN_ALWAYS, _file);
				if (!s.ok())
					throw IOException(s.ToString().c_str());
				if (_file != INVALID_HANDLE_VALUE && GetFileInformationByHandleEx(_file, FILE_INFO_BY_HANDLE_ref class::FileStandardInfo, &fi, sizeof(fi)))
				{
					_fileSizeLow = fi.EndOfFile.LowPart;
					_fileSizeHigh = fi.EndOfFile.HighPart;
					if (_fileSizeLow > 0 || _fileSizeHigh > 0)
					{
                        OVERLAPPED overlapped = { };
						if (!::LockFileEx(_file, 0, 0, _fileSizeLow, _fileSizeHigh, &overlapped))
						{
							Status s = GetLastWindowsError(fname);
							throw IOException(s.ToString().c_str());
						}
					}
				}
				else
				{
					_fileSizeLow = _fileSizeHigh = 0;
				}
			}

			~WinFileLock() 
			{
				if (_file != INVALID_HANDLE_VALUE)
				{
					if (_fileSizeLow > 0 || _fileSizeHigh > 0)
						if (!::UnlockFileEx(_file, 0, _fileSizeLow, _fileSizeHigh, NULL))
						{
							Status s = GetLastWindowsError(_fname);
						}
					CloseFile(_fname, _file);
				}
			}

		};

		ref class WinRTEnv : public Env {
		public:
			WinRTEnv();
			
			virtual ~WinRTEnv() {
				fprintf(stderr, "Destroying Env::Default()\n");
			}

			virtual Status NewSequentialFile(const System::String& fname, SequentialFile** result)
			{
				Status s;
				try {
					*result = new WinSequentialFile(fname);
				}
				catch (const IOException & e) {
					s = Status::IOError(fname, e.what());
				}
				return s;
			}

			virtual Status NewRandomAccessFile(const System::String& fname, RandomAccessFile** result)
			{
				Status s;
				try {
					*result = new WinRandomAccessFile(fname);
				}
				catch (const IOException & e) {
					s = Status::IOError(fname, e.what());
				}
				return s;
			}

			virtual Status NewWritableFile(const System::String& fname, WritableFile** result) {
				Status s;
				try {
					// will create a new empty file to write to
					*result = new WinFile(fname);
				} catch(const IOException & e) {
					s = Status::IOError(fname, e.what());
				}
				return s;
			}

			virtual bool FileExists(const System::String& fname) {
				WIN32_FILE_ATTRIBUTE_DATA fi;
				return (GetFileAttributesExW(GetFullPath(fname).c_str(), GET_FILEEX_INFO_LEVELS::GetFileExInfoStandard, &fi) ? true : false);
			}

			virtual Status GetChildren(const System::String& dir, std::vector<System::String>* result) {
				System::String path = dir;
				result->clear();

				WIN32_FIND_DATAW ffd;
				HANDLE hFind;
				path = dir + "/*";
				hFind = FindFirstFileExW(GetFullPath(path).c_str(), FINDEX_INFO_LEVELS::FindExInfoStandard, &ffd, FINDEX_SEARCH_OPS::FindExSearchNameMatch, NULL, 0);

				if(INVALID_HANDLE_VALUE == hFind) {
					return GetLastWindowsError(path);
				}

				do {
					result->push_back(ws2s(ffd.cFileName));
				} while(FindNextFileW(hFind, &ffd) != 0);

				FindClose(hFind);

				return Status::OK();
			}

//Deletefile has become a macro for unicode
#pragma push_macro("DeleteFile")
#undef DeleteFile

			// some actions
			virtual Status DeleteFile(const System::String& fname) {
				if (::DeleteFileW(GetFullPath(fname).c_str()) != 0) {
					return Status::OK();
				} else {
					return GetLastWindowsError(fname);
				}
			}

#pragma pop_macro("DeleteFile")

			virtual Status CreateDir(const System::String& name) {
				EnsureDirectory(name);
				::CreateDirectoryW(GetFullPath(name).c_str(), NULL);
				return Status::OK();
			};

			virtual Status DeleteDir(const System::String& name) {
				BOOL ret = ::RemoveDirectoryW(GetFullPath(name).c_str());
				if (!ret)
					Status s = GetLastWindowsError(name);
				return Status::OK();
			};

			virtual Status GetFileSize(const System::String& fname, uint64_t* size) {
				WIN32_FILE_ATTRIBUTE_DATA fi;
				BOOL ret = GetFileAttributesExW(GetFullPath(fname).c_str(), GET_FILEEX_INFO_LEVELS::GetFileExInfoStandard, &fi);
				if (!ret)
					return GetLastWindowsError(fname);
				*size = ((uint64_t)fi.nFileSizeLow + ((uint64_t)fi.nFileSizeHigh << 32));
				return Status::OK();
			}

			virtual Status RenameFile(const System::String& src, const System::String& target) {
				std::wstring fullsrc = GetFullPath(src);
				std::wstring fulltarget = GetFullPath(target);
				::DeleteFileW(fulltarget.c_str());
				if (::MoveFileExW(fullsrc.c_str(), fulltarget.c_str(), 0) != TRUE) {
					return GetLastWindowsError(src);
				} else {
					return Status::OK();
				}
			}

			virtual Status LockFile(const System::String& fname, FileLock** lock) {
				*lock = NULL;
				if (!FileExists(fname)) {
					HANDLE file;
					Status s = OpenFile(fname, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, CREATE_ALWAYS, file);
					if (s.ok())
						CloseFile(fname, file);
				}
				try
				{
					*lock = new WinFileLock(fname);
				}
				catch (const IOException & e) {
					return Status::IOError(fname, e.what());
				}

				return Status::OK();
			}

			virtual Status UnlockFile(FileLock* lock) {
				delete lock;
				return Status::OK();
			}

			virtual void Schedule(void(*function)(void*), void* arg);

			virtual void StartThread(void(*function)(void* arg), void* arg);

			virtual Status GetTestDirectory(System::String* result) {
				System::Stringstream ss;
				ss << "tmp/leveldb_tests/" << current_process_id();

				// Directory may already exist
				CreateDir(ss.str());

				*result = ss.str();

				return Status::OK();
			}

#ifndef WIN32
			static uint64_t gettid() {
				pthread_t tid = pthread_self();
				uint64_t thread_id = 0;
				memcpy(&thread_id, &tid, std::min(sizeof(thread_id), sizeof(tid)));
				return thread_id;
			}
#endif

			virtual Status NewLogger(const System::String& fname, Logger** result) {
				*result = new NoOpLogger();
				return Status::OK();
			}

			virtual uint64_t NowMicros() {
				const auto now = std::chrono::high_resolution_clock::now().time_since_epoch();

				return std::chrono::duration_cast<std::chrono::microseconds>(now).count();
			}

			virtual void SleepForMicroseconds(int micros) {
				std::this_thread::sleep_for(std::chrono::microseconds(micros));
			}


		private:

			// BGThread() is the body of the background thread
			void BGThread();

			static void BGThreadWrapper(void* arg) {
				reinterpret_cast<WinRTEnv*>(arg)->BGThread();
			}

			std::mutex mu_;
			std::condition_variable bgsignal_;
			std::unique_ptr<std::thread> bgthread_;

			// Entry per Schedule() call
			ref struct BGItem { void* arg; void(*function)(void*); };
			typedef std::deque<BGItem> BGQueue;
			BGQueue queue_;
		};

		WinRTEnv::WinRTEnv() {}

		void WinRTEnv::Schedule(void(*function)(void*), void* arg) {
			std::unique_lock<std::mutex> lock(mu_);

			// Start background thread if necessary
			if(!bgthread_) {
				bgthread_.reset(
					new std::thread(&BGThreadWrapper, this));
			}

			// Add to priority queue
			queue_.push_back(BGItem());
			queue_.back().function = function;
			queue_.back().arg = arg;

			lock.unlock();

			bgsignal_.notify_one();

		}

		void WinRTEnv::BGThread() {
			while(true) {
				// Wait until there is an item that is ready to run
				std::unique_lock<std::mutex> lock(mu_);

				while(queue_.empty()) {
					bgsignal_.wait(lock);
				}

				void(*function)(void*) = queue_.front().function;
				void* arg = queue_.front().arg;
				queue_.pop_front();

				lock.unlock();
				(*function)(arg);
			}
		}

		namespace {
			ref struct StartThreadState {
				void(*user_function)(void*);
				void* arg;
			};
		}

		DWORD WINAPI StartThreadWrapper(LPVOID lpParam) {
			StartThreadState* state = reinterpret_cast<StartThreadState*>(lpParam);
			state->user_function(state->arg);
			delete state;
			return 0;
		}
 
		void WinRTEnv::StartThread(void(*function)(void* arg), void* arg) {
 			StartThreadState* state = new StartThreadState;
 			state->user_function = function;
 			state->arg = arg;
			_Thrd_t thrd;
			DWORD     thread_id;

			CreateThread(
				NULL,                   // default security attributes
				0,                      // use default stack size  
				StartThreadWrapper,       // thread function name
				state,          // argument to thread function 
				0,                      // use default creation flags 
				&thread_id);   // returns the thread identifier 
 		}
	}

	static INIT_ONCE g_InitOnce = INIT_ONCE_STATIC_INIT;
	static Env* default_env;
	static BOOL CALLBACK InitDefaultEnv(PINIT_ONCE InitOnce,
		PVOID Parameter,
		PVOID *lpContext) {
		default_env = new WinRTEnv;
		return TRUE;
	}

	Env* Env::Default() {
#if 0
		PVOID lpContext;
		InitOnceExecuteOnce(&g_InitOnce,          // One-time initialization structure
			InitDefaultEnv,   // Pointer to initialization callback function
			"",                 // Optional parameter to callback function (not used)
			&lpContext);          // Receives pointer to event object stored in g_InitOnce
#else
		if (default_env == NULL)
			InitDefaultEnv(NULL, NULL, NULL);
#endif

		return default_env;
	}
}
#endif
