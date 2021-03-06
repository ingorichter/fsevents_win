/*
 * Copyright (c) 2014 Adobe Systems Incorporated. All rights reserved.
 *  
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"), 
 * to deal in the Software without restriction, including without limitation 
 * the rights to use, copy, modify, merge, publish, distribute, sublicense, 
 * and/or sell copies of the Software, and to permit persons to whom the 
 * Software is furnished to do so, subject to the following conditions:
 *  
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *  
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, 
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER 
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING 
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER 
 * DEALINGS IN THE SOFTWARE.
 * 
 */ 

#include <v8.h>
#include <uv.h>
#include <node.h>
#include <nan.h>

#include <node_object_wrap.h>

#include <string>
#include <queue>

#define READ_DIRECTORY_CHANGES_BUFSIZE  8192

using namespace v8;

namespace fswatch_win {
	// encapsulates changes reported by ReadDirectoryChangesW()
	class CFileNotifyChangeInfo
	{
	public:
		// records a single change as reported by ReadDirectoryChanges()
		struct Entry {
			std::wstring m_wstrFilename;
			DWORD m_dwAction;

			Entry(LPWSTR lpwszFilename, int iFilenameLen, DWORD dwAction);
		};
		typedef Entry* LPEntry;

	public:
		CFileNotifyChangeInfo();
		~CFileNotifyChangeInfo();

		// mutex-synchronized accessor methods
		void Push(LPEntry lpEntry);				// pushes a new entry to the tail of the queue
		LPEntry Peek();							// retrieves the entry at the head of the queue
		void Pop();								// pops the entry off the head of the queue

	protected:
		std::queue<LPEntry> m_Entries;			// queue of ReadDirectoryChangesW() entries
		HANDLE m_hMutex;						// mutex synchronizing access to queue
	};

	// encapsulates data to be shared with the worker thread
	struct THREADINFO
	{
		CFileNotifyChangeInfo *m_pChangeInfo;	// queue of file notification changes
		PFILE_NOTIFY_INFORMATION m_lpBuffer;	// buffer for watched file notification changes
		HANDLE m_hAsyncDir;						// handle to watched parent folder
		HANDLE m_hIoCPort;						// handle to I/O completion port
		LPOVERLAPPED m_lpOverlapped;			// handle to overlapped I/O
		uv_async_t *m_lpuvaWatcher;				// handle to uv watch
	};
	typedef THREADINFO *LPTHREADINFO;

	// main node object
	class NodeFSEvents : public node::ObjectWrap
	{
	public:
		NodeFSEvents(const char *lpszPath);
		~NodeFSEvents();

		// JS wrapper methods
		static void Initialize(Handle<Object> exports);
		static NAN_METHOD(New);
		static NAN_METHOD(Shutdown);

	protected:
		// file watching methods
		void Startup(const char *lpszPath);
		void Shutdown();

	public:
		static DWORD WINAPI Run(LPVOID lpData);
		static void Callback(uv_async_t *async_data);

	protected:
		std::wstring m_wstrRootPath;			// root path to watch
		CFileNotifyChangeInfo *m_pChangeInfo;	// queue of file notification changes
		PFILE_NOTIFY_INFORMATION m_lpBuffer;	// buffer for watched file notification changes
		HANDLE m_hAsyncDir;						// handle to watched parent folder
		HANDLE m_hIoCPort;						// handle to I/O completion port
		OVERLAPPED m_Overlapped;				// handle to overlapped I/O
		uv_async_t m_uvaWatcher;				// handle to uv watch
		HANDLE m_hThread;						// handle to watcher thread
		LPTHREADINFO m_lpThreadInfo;			// info to be shared with the worker thread

	private:
		static v8::Persistent<v8::FunctionTemplate> constructor;
		void Emit(int argc, Handle<Value> argv[]);
	};
}