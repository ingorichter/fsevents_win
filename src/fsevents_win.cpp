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

/* This implementation based on code originally written by
 *   (c) 2013 by Philipp Dunkel <p.dunkel@me.com>. Licensed under MIT License
 *   https://github.com/phidelta/fsevents.git
 */

#include "fsevents_win.h"

#define WIN32_LEAN_AND_MEAN			// Exclude rarely-used stuff from Windows headers
#include <windows.h>

#include <algorithm>

//#pragma comment(lib, "node")		// link to node

static Persistent<FunctionTemplate> constructor_template;
static Persistent<String> emit_sym;
static Persistent<String> change_sym;

using namespace v8;
using namespace fswatch_win;
//*****************************************************************************

// node addon required function
extern "C" void init(Handle<Object> exports) {
	NodeFSEvents::Initialize(exports);
}

NODE_MODULE(fswatch_win, init)

//*****************************************************************************

// wraps gaining access to a given mutex
class CMutexAccess
{
public:
	CMutexAccess(HANDLE hMutex) : m_hMutex(hMutex)
	{
		if (m_hMutex != NULL)
			::WaitForSingleObject(m_hMutex, INFINITE);
	}

	~CMutexAccess()
	{
		if (m_hMutex != NULL)
			::ReleaseMutex(m_hMutex);
	}

private:
	HANDLE m_hMutex;
};

//*****************************************************************************

// constructor
CFileNotifyChangeInfo::Entry::Entry(LPWSTR lpFilename, int iFilenameLen, DWORD dwAction) :
	m_wstrFilename(lpFilename, iFilenameLen),
	m_dwAction(dwAction)
{
}

// constructor
CFileNotifyChangeInfo::CFileNotifyChangeInfo()
{
	// mutex to synchronize access to the queue
	m_hMutex = ::CreateMutexA(NULL, FALSE, NULL);
}

// destructor
CFileNotifyChangeInfo::~CFileNotifyChangeInfo()
{
	// cleanup queue
	{
		CMutexAccess access(m_hMutex);
		while (!m_Entries.empty())
		{
			LPEntry lpEntry = m_Entries.front();
			m_Entries.pop();
			delete lpEntry;
		}
	}

	// cleanup
	::CloseHandle(m_hMutex);
}

// pushes a new entry to the tail of the queue
void CFileNotifyChangeInfo::Push(LPEntry lpEntry)
{
	if (lpEntry != NULL)
	{
		CMutexAccess access(m_hMutex);
		m_Entries.push(lpEntry);
	}
}

// retrieves the entry at the head of the queue
CFileNotifyChangeInfo::LPEntry CFileNotifyChangeInfo::Peek()
{
	LPEntry lpEntry = NULL;

	CMutexAccess access(m_hMutex);
	if (!m_Entries.empty())
		lpEntry = m_Entries.front();

	return lpEntry;
}

// pops the entry off the head of the queue
void CFileNotifyChangeInfo::Pop()
{
	CMutexAccess access(m_hMutex);
	if (!m_Entries.empty())
		m_Entries.pop();
}


//*****************************************************************************

// constructor
NodeFSEvents::NodeFSEvents(const char *lpszPath) : ObjectWrap(),
	m_pChangeInfo(NULL),
	m_lpBuffer(NULL),
	m_hAsyncDir(NULL),
	m_hIoCPort(NULL),
	m_hThread(NULL),
	m_lpThreadInfo(NULL)
{
	Startup(lpszPath);
}

// destructor
NodeFSEvents::~NodeFSEvents()
{
	Shutdown();
}

Persistent<FunctionTemplate> NodeFSEvents::constructor;

// initialize a JS wrapper around this object
void NodeFSEvents::Initialize(Handle<Object> exports)
{
	NanScope();
    
//	emit_sym = NODE_PSYMBOL("emit");
//	change_sym = NODE_PSYMBOL("fsevent");
	Local<FunctionTemplate> tpl = NanNew<FunctionTemplate>(New);
	tpl->SetClassName(NanNew("FSEvents"));
	tpl->InstanceTemplate()->SetInternalFieldCount(1);

	NODE_SET_PROTOTYPE_METHOD(tpl, "shutdown", Shutdown);

	exports->Set(NanNew("FSEvents"), tpl->GetFunction());
	NanAssignPersistent(constructor, tpl);
}

// API to allocate a new object from JS
NAN_METHOD(NodeFSEvents::New)
//Handle<Value> NodeFSEvents::New(const v8::FunctionCallbackInfo<v8::Value>& args)
{
	NanScope();

	if (args.Length() != 1 || !args[0]->IsString()) {
		return NanThrowError("Bad arguments");
	}

	String::Utf8Value szPathName(args[0]->ToString());

//	NanUtfString* szPathName = new NanUtf8String(args[0]);

	NodeFSEvents *nodeFSE = new NodeFSEvents(*szPathName);
	nodeFSE->Wrap(args.This());
//	NODE_SET_METHOD(args.Holder(), "stop", NodeFSEvents::Shutdown);

	NanReturnValue(args.This());
}

// unwrap the JS wrapper
NAN_METHOD(NodeFSEvents::Shutdown)
//Handle<Value> NodeFSEvents::Shutdown(const v8::FunctionCallbackInfo<v8::Value>& args)
{
	NanScope();
    
	NodeFSEvents *native = node::ObjectWrap::Unwrap<NodeFSEvents>(args.This());
	native->Shutdown();
	
    NanReturnUndefined();
}

// starts up the file watching thread
void NodeFSEvents::Startup(const char *lpszPath)
{
	if (m_hThread != NULL)		// check if already started
		return;

	BOOL bResult = FALSE;

	// validate path input
	int iLen = ::MultiByteToWideChar(CP_UTF8, 0, lpszPath, -1, NULL, 0);
	m_wstrRootPath.resize(iLen + 1);
	::MultiByteToWideChar(CP_UTF8, 0, lpszPath, -1, &m_wstrRootPath[0], iLen + 1);

	// allocate buffer to hold asynchronous watch info
	m_pChangeInfo = new CFileNotifyChangeInfo();
	if (m_pChangeInfo != NULL)
	{
		// allocate buffer to hold directory change information
		m_lpBuffer = (PFILE_NOTIFY_INFORMATION)malloc(READ_DIRECTORY_CHANGES_BUFSIZE);
		if (m_lpBuffer != NULL)
		{
			// open the directory to watch
			m_hAsyncDir = CreateFileW(
				m_wstrRootPath.c_str(),
				FILE_LIST_DIRECTORY,
				FILE_SHARE_READ | FILE_SHARE_WRITE,
				NULL,
				OPEN_EXISTING,
				FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED,
				NULL);
			if (m_hAsyncDir != INVALID_HANDLE_VALUE)
			{
				// set up communication channels between watcher and our thread
				m_hIoCPort = ::CreateIoCompletionPort(
					m_hAsyncDir,
					m_hIoCPort,
					(ULONG_PTR)m_pChangeInfo,
					0);
				if (m_hIoCPort != NULL)
				{
					memset(&m_Overlapped, 0, sizeof(m_Overlapped));

					// set up communication between our file watching thread and a callback to post changes back to JS
					uv_async_init(uv_default_loop(), &m_uvaWatcher, NodeFSEvents::Callback);
					m_uvaWatcher.data = (LPVOID)this;

					// collect info to be shared with the worker thread
					m_lpThreadInfo = (LPTHREADINFO)malloc(sizeof(THREADINFO));
					if (m_lpThreadInfo != NULL)
					{
						m_lpThreadInfo->m_pChangeInfo = m_pChangeInfo;
						m_lpThreadInfo->m_hAsyncDir = m_hAsyncDir;
						m_lpThreadInfo->m_hIoCPort = m_hIoCPort;
						m_lpThreadInfo->m_lpBuffer = m_lpBuffer;
						m_lpThreadInfo->m_lpOverlapped = &m_Overlapped;
						m_lpThreadInfo->m_lpuvaWatcher = &m_uvaWatcher;

						// start the thread
						DWORD dwThreadId = 0;
						m_hThread = ::CreateThread(NULL, 0, &NodeFSEvents::Run, (LPVOID)m_lpThreadInfo, 0, &dwThreadId);

						bResult = m_hThread != NULL;
					}
				}
			}
		}
	}

	// if something failed, then shutdown and cleanup
	if (!bResult)
		Shutdown();
}

// cleanly shuts down the file watching thread
void NodeFSEvents::Shutdown()
{
	// shutdown the thread, if it's running
	if (m_hThread != NULL)
	{
		::PostQueuedCompletionStatus(m_hIoCPort, 0, 0, NULL);   // tell the thread to quit
		::WaitForSingleObject(m_hThread, INFINITE);				// wait for the thread to quit
		uv_close((uv_handle_t*) &m_uvaWatcher, NULL);
		m_hThread = NULL;
	}

	// cleanup
	if (m_lpThreadInfo != NULL)
	{
		free(m_lpThreadInfo);
		m_lpThreadInfo = NULL;
	}
	if (m_hIoCPort != NULL)
	{
		::CloseHandle(m_hIoCPort);
		m_hIoCPort = NULL;
	}
	if (m_hAsyncDir != NULL)
	{
		::CloseHandle(m_hAsyncDir);
		m_hAsyncDir = NULL;
	}
	if (m_lpBuffer != NULL)
	{
		::free(m_lpBuffer);
		m_lpBuffer = NULL;
	}
	if (m_pChangeInfo != NULL)
	{
		delete m_pChangeInfo;
		m_pChangeInfo = NULL;
	}
}

// threadproc used to maintain the file watcher and any file changes it finds
DWORD WINAPI NodeFSEvents::Run(LPVOID lpData)
{
	LPTHREADINFO lpThreadInfo = (LPTHREADINFO)lpData;
	if (lpThreadInfo != NULL)
	{
		// iterate watching for directory changes until we're signalled to stop
		BOOL bContinue = TRUE;
		while (bContinue)
		{
			// set the watcher
			DWORD dwBytesReturned = (DWORD)0;
			bContinue = ::ReadDirectoryChangesW(lpThreadInfo->m_hAsyncDir, lpThreadInfo->m_lpBuffer, READ_DIRECTORY_CHANGES_BUFSIZE, TRUE, 
				FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME | FILE_NOTIFY_CHANGE_ATTRIBUTES | FILE_NOTIFY_CHANGE_SIZE
				| FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_LAST_ACCESS | FILE_NOTIFY_CHANGE_CREATION | FILE_NOTIFY_CHANGE_SECURITY,
				&dwBytesReturned, lpThreadInfo->m_lpOverlapped, NULL);

			if (bContinue)
			{
				// wait for a signal
				DWORD dwNumBytes;
				LPOVERLAPPED lpOverlapped = NULL;
				ULONG_PTR lpCompletionKey = NULL; 
				::GetQueuedCompletionStatus(lpThreadInfo->m_hIoCPort, &dwNumBytes, &lpCompletionKey, &lpOverlapped, INFINITE);
				if (dwNumBytes > 0)
				{
					if (lpOverlapped == NULL)
					{
						// in case an unknown error occurred in the completion port, skip to the next change notification
						continue;
					}
					else
					{
						// process the watched directory change
						PFILE_NOTIFY_INFORMATION pfni = lpThreadInfo->m_lpBuffer;
						while (pfni != NULL)
						{
							CFileNotifyChangeInfo::LPEntry lpEntry = new CFileNotifyChangeInfo::Entry(
								pfni->FileName,
								pfni->FileNameLength / sizeof(WCHAR),
								pfni->Action);
							if (lpEntry != NULL && (lpThreadInfo->m_pChangeInfo != NULL))
							{
								lpThreadInfo->m_pChangeInfo->Push(lpEntry);

								// post a signal to send this back to JS
								uv_async_send(lpThreadInfo->m_lpuvaWatcher);
							}

							// process the next change
							pfni = (pfni->NextEntryOffset > 0) ? (PFILE_NOTIFY_INFORMATION)(((BYTE*)pfni) + pfni->NextEntryOffset) : NULL;
						}
					}
				}
				else
				{
					// signaled from Shutdown() to end this thread
					bContinue = FALSE;
				}
			}
		}
	}

	return TRUE;
}

// callback signaled from file watching threadproc that allows us to asynchronously post changes back to JS
void NodeFSEvents::Callback(uv_async_t *async_data)
{
	NodeFSEvents *This = static_cast<NodeFSEvents*>(async_data->data);
	if (This->m_pChangeInfo != NULL)
	{
        NanScope();

		// initialize wrapper to call back into JS
//		Local<Value> callback_v = This->handle_->Get(emit_sym);
//		Local<Function> callback = Local<Function>::Cast(callback_v);
		Handle<Value> args[3];
		args[0] = NanNew("fsevent");
        
		// iterate thru each queued file watching entry
		CFileNotifyChangeInfo::LPEntry lpEntry = NULL;
		while ((lpEntry = This->m_pChangeInfo->Peek()) != NULL)
		{
			// concatenate the root search path and the file notification path
			std::wstring wstrFullPath = This->m_wstrRootPath.c_str();
			wstrFullPath += lpEntry->m_wstrFilename;

			// normalize path separators from '\' to '/'
			std::replace(wstrFullPath.begin(), wstrFullPath.end(), '\\', '/');

			// convert the full pathname to utf8
			int iLen = ::WideCharToMultiByte(CP_UTF8, 0, wstrFullPath.c_str(), -1, NULL, 0, NULL, NULL);
			std::string strFullPath(iLen + 1, 0x00);
			::WideCharToMultiByte(CP_UTF8, 0, wstrFullPath.c_str(), -1, &strFullPath[0], iLen + 1, NULL, NULL);

			// map the FILE_NOTIFY_INFORMATION 'Action' to a node fs-event.c 'enum uv_fs_event' type
			int iAction;
			switch(lpEntry->m_dwAction)
			{
			case FILE_ACTION_ADDED:
			case FILE_ACTION_REMOVED:
			case FILE_ACTION_RENAMED_OLD_NAME:
			case FILE_ACTION_RENAMED_NEW_NAME:
				iAction = UV_RENAME;
				break;
			case FILE_ACTION_MODIFIED:
			default:
				iAction = UV_CHANGE;
			}

			// call back into JS with each change.
			args[1] = NanNew(strFullPath.c_str());
			args[2] = NanNew(iAction);
			This->Emit(3, args);

			// discard the now-processed entry
			This->m_pChangeInfo->Pop();
			delete lpEntry;
		}
	}
}

void NodeFSEvents::Emit(int argc, Handle<Value> argv[])
{
    NanScope();

    Handle<Object> handle = NanObjectWrapHandle(this);
    Local<Function> emit = handle->Get(NanNew("emit")).As<Function>();
    emit->Call(handle, argc, argv);
 }