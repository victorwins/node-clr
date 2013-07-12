#include "node-clr.h"

using namespace v8;
using namespace System::Collections::Generic;
using namespace System::Dynamic;
using namespace System::Linq;
using namespace System::Reflection;


System::Object^ CLRBinder::InvokeConstructor(
	Handle<Value> typeName,
	const Arguments& args)
{
	auto type = CLRGetType(ToCLRString(typeName));

	auto arr = Array::New();
	for (int i = 0; i < args.Length(); i++)
	{
		arr->Set(Number::New(i), args[i]);
	}

	return InvokeConstructor(
		type,
		arr);
}

System::Object^ CLRBinder::InvokeConstructor(
	System::Type^ type,
	Handle<Array> args)
{
	if (args->Length() == 0)
	{
		return System::Activator::CreateInstance(type);
	}
	else
	{
		auto ctors = type->GetConstructors();

		auto ctor = (ConstructorInfo^)SelectMethod(
			Enumerable::ToArray(Enumerable::Cast<MethodBase^>(ctors)),
			args);
		return ctor->Invoke(
			BindingFlags::OptionalParamBinding,
			nullptr,
			BindToMethod(ctor, args),
			nullptr);
	}
}

Handle<Value> CLRBinder::InvokeMethod(
	Handle<Value> typeName,
	Handle<Value> name,
	Handle<Value> target,
	Handle<Value> args)
{
	auto type = CLRGetType(ToCLRString(typeName));

	return InvokeMethod(
		type,
		ToCLRString(name),
		(CLRObject::IsWrapped(target))
			? CLRObject::Unwrap(target)
			: nullptr,
		Handle<Array>::Cast(args));
}

Handle<Value> CLRBinder::InvokeMethod(
	System::Type^ type,
	System::String^ name,
	System::Object^ target,
	Handle<Array> args)
{
	auto methods = type->GetMethods(
		BindingFlags::Public |
		((target != nullptr) ? BindingFlags::Instance : BindingFlags::Static));
	
	auto match = gcnew List<MethodBase^>();
	for each (auto method in methods)
	{
		if (name == method->Name)
		{
			match->Add(method);
		}
	}

	auto method = (MethodInfo^)SelectMethod(
		match->ToArray(),
		args);

	auto result = method->Invoke(
		target,
		BindingFlags::OptionalParamBinding,
		nullptr,
		BindToMethod(method, args),
		nullptr);
	if (result == nullptr &&
		method->ReturnType == System::Void::typeid)
	{
		return Undefined();
	}
	else
	{
		return ToV8Value(result);
	}
}

Handle<Value> CLRBinder::GetField(
	Handle<Value> typeName,
	Handle<Value> name,
	Handle<Value> target)
{
	auto type = CLRGetType(ToCLRString(typeName));

	return GetField(
		type,
		ToCLRString(name),
		(CLRObject::IsWrapped(target))
			? CLRObject::Unwrap(target)
			: nullptr);
}

Handle<Value> CLRBinder::GetField(
	System::Type^ type,
	System::String^ name,
	System::Object^ target)
{
	auto fi = type->GetField(name);
	auto result = fi->GetValue(target);
	return ToV8Value(result);
}

void CLRBinder::SetField(
	Handle<Value> typeName,
	Handle<Value> name,
	Handle<Value> target,
	Handle<Value> value)
{
	auto type = CLRGetType(ToCLRString(typeName));

	SetField(
		type,
		ToCLRString(name),
		(CLRObject::IsWrapped(target))
			? CLRObject::Unwrap(target)
			: nullptr,
		value);
}

void CLRBinder::SetField(
	System::Type^ type,
	System::String^ name,
	System::Object^ target,
	Handle<Value> value)
{
	auto fi = type->GetField(name);
	fi->SetValue(
		target,
		ChangeType(value, fi->FieldType));
}

MethodBase^ CLRBinder::SelectMethod(
	array<MethodBase^>^ methods,
	Handle<Array> args)
{
	if (methods->Length == 0)
	{
		throw gcnew System::MissingMethodException();
	}

	auto scores = gcnew array<int>(methods->Length);
	for (int i = 0; i < methods->Length; i++)
	{
		BindToMethod(methods[i], args, scores[i]);
	}

	int max = Enumerable::Max(scores);
	if (max < IMPLICIT_CONVERSION)
	{
		throw gcnew System::MissingMethodException();
	}

	auto canditates = gcnew List<MethodBase^>();
	for (int i = 0; i < methods->Length; i++)
	{
		if (scores[i] == max)
		{
			canditates->Add(methods[i]);
		}
	}

	return FindMostSpecificMethod(canditates->ToArray(), args);
}

array<System::Object^>^ CLRBinder::BindToMethod(
	MethodBase^ method,
	Handle<Array> args)
{
	int match;
	auto result = BindToMethod(
		method,
		args,
		match);
	if (INCOMPATIBLE < match)
	{
		return result;
	}
	else
	{
		throw gcnew System::MissingMethodException();
	}
}

array<System::Object^>^ CLRBinder::BindToMethod(
	MethodBase^ method,
	Handle<Array> args,
	int% match)
{
	auto params = method->GetParameters();

	// check for ref or out parameter
	for each (ParameterInfo^ param in params)
	{
		if (param->ParameterType->IsByRef)
		{
			match = INCOMPATIBLE;
			return nullptr;
		}
	}
	
	// check for parameter count
	int paramsMin = 0, paramsMax = 0;
	bool isVarArgs = false;
	for each (ParameterInfo^ param in params)
	{
		if (0 < param->GetCustomAttributes(System::ParamArrayAttribute::typeid, false)->Length)
		{
			paramsMax = int::MaxValue;
			isVarArgs = true;
		}
		else if (param->IsOptional)
		{
			paramsMax++;
		}
		else
		{
			paramsMin++;
			paramsMax++;
		}
	}
	if ((int)args->Length() < paramsMin ||
		paramsMax < (int)args->Length())
	{
		match = INCOMPATIBLE;
		return nullptr;
	}

	// get varargs type
	System::Type^ varArgsType = nullptr;
	if (isVarArgs)
	{
		auto paramType = params[params->Length - 1]->ParameterType;
		if (paramType->IsArray && paramType->HasElementType)
		{
			varArgsType = paramType->GetElementType();
		}
		else if (paramType->IsGenericType)
		{
			varArgsType = paramType->GetGenericArguments()[0];
		}
		else
		{
			varArgsType = System::Object::typeid;
		}
	}

	// bind parameters
	match = EXACT;
	auto arguments = gcnew array<System::Object^>(System::Math::Min((int)args->Length(), params->Length));
	for (int i = 0; i < (int)args->Length(); i++)
	{
		if (isVarArgs &&
			i == params->Length - 1 &&
			i == (int)args->Length() - 1)
		{
			int score1;
			auto arg1 = ChangeType(args->Get(Number::New(i)), params[i]->ParameterType, score1);
			int score2;
			auto arg2 = ChangeType(args->Get(Number::New(i)), varArgsType, score2);

			if (score1 >= score2)
			{
				arguments[i] = arg1;
				match = System::Math::Min(match, score1);
			}
			else
			{
				auto arr = System::Array::CreateInstance(varArgsType, 1);
				arr->SetValue(arg2, 0);
				arguments[i] = arr;
				match = System::Math::Min(match, score2);
			}
		}
		else if (i < params->Length)
		{
			int s;
			arguments[i] = ChangeType(args->Get(Number::New(i)), params[i]->ParameterType, s);

			match = System::Math::Min(match, s);
		}
		else
		{
			int s;
			auto arg =  ChangeType(args->Get(Number::New(i)), varArgsType, s);

			System::Array^ arr;
			if (arguments[arguments->Length - 1] == nullptr)
			{
				arr = System::Array::CreateInstance(varArgsType, args->Length() - params->Length + 1);
				arguments[arguments->Length - 1] = arr;
			}
			else
			{
				arr = (System::Array^)arguments[arguments->Length - 1];
			}

			arr->SetValue(arg, i - params->Length + 1);
			match = System::Math::Min(match, s);
		}
	}

	return arguments;
}

MethodBase^ CLRBinder::FindMostSpecificMethod(
	array<MethodBase^>^ methods,
	Handle<Array> args)
{
	auto current = methods[0];
	for (int i = 1; i < methods->Length; i++)
	{
		if (0 < CompareMethods(current, methods[i]))
		{
			current = methods[i];
		}
	}
	return current;
}

int CLRBinder::CompareMethods(MethodBase^ lhs, MethodBase^ rhs)
{
	auto params1 = lhs->GetParameters();
	auto params2 = rhs->GetParameters();
	
	auto count = System::Math::Min(params1->Length, params2->Length);
	for (int i = 0; i < count; i++)
	{
		int c = CompareTypes(params1[i]->ParameterType, params2[i]->ParameterType);
		if (c != 0)
		{
			return c;
		}
	}

	return 0;
}

int CLRBinder::CompareTypes(System::Type^ lhs, System::Type^ rhs)
{
	if (lhs == rhs)
	{
		return 0;
	}
	else
	{
		if (lhs->IsAssignableFrom(rhs))
		{
			return 1;
		}
		else if (rhs->IsAssignableFrom(lhs))
		{
			return -1;
		}

		// compare primitive types
		if (lhs->IsPrimitive && rhs->IsPrimitive)
		{
			return (int)System::Type::GetTypeCode(rhs) - (int)System::Type::GetTypeCode(lhs);
		}

		// compare array types
		if (lhs->IsArray && rhs->IsArray)
		{
			return CompareTypes(lhs->GetElementType(), rhs->GetElementType());
		}
		if (lhs->IsGenericType && rhs->IsGenericType &&
			lhs->GetGenericTypeDefinition() == rhs->GetGenericTypeDefinition())
		{
			auto typeParams1 = lhs->GetGenericArguments();
			auto typeParams2 = rhs->GetGenericArguments();

			for (int i = 0; i < typeParams1->Length; i++)
			{
				int c = CompareTypes(typeParams1[i], typeParams2[i]);
				if (c != 0)
				{
					return c;
				}
			}
		}

		// compare delegate types
		if (System::Delegate::typeid->IsAssignableFrom(lhs) &&
			System::Delegate::typeid->IsAssignableFrom(rhs))
		{
			auto params1 = lhs->GetMethod("Invoke")->GetParameters();
			auto params2 = rhs->GetMethod("Invoke")->GetParameters();

			int c = params2->Length - params1->Length;
			if (c != 0)
			{
				return c;
			}

			for (int i = 0; i < params1->Length; i++)
			{
				c = CompareTypes(params1[i]->ParameterType, params2[i]->ParameterType);
				if (c != 0)
				{
					return c;
				}
			}
		}

		return 0;
	}
}
