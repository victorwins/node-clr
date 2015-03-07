#ifndef V8FUNCTION_H_
#define V8FUNCTION_H_

#include "node-clr.h"


class V8Function
{
private:
	struct InvocationContext
	{
		gcroot<array<System::Object^>^> args;
		gcroot<System::Object^> result;
		gcroot<System::Exception^> exception;
		uv_sem_t completed;
	};

private:
	uv_thread_t threadId;
	v8::Persistent<v8::Function> function;
	uv_async_t async;
	uv_mutex_t lock;
	std::queue<InvocationContext*> invocations;
	bool terminate;

public:
	static V8Function* New(v8::Handle<v8::Function> func);
	System::Object^ Invoke(array<System::Object^>^ args);
	void Destroy();

private:
	V8Function(v8::Handle<v8::Function> func);
	System::Object^ InvokeImpl(array<System::Object^>^ args);
	System::Object^ InvokeAsync(array<System::Object^>^ args);
	static void AsyncCallback(uv_async_t* handle);
	~V8Function();
};

#endif
