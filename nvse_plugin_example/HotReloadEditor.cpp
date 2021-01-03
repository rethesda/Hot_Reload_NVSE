#include "SocketUtils.h"
#include "GameAPI.h"
#include "SafeWrite.h"
#include <thread>

#include "GameData.h"
#include "HotReloadUtils.h"
#include "ScriptTokenCache.h"

typedef void (__cdecl* _EditorLog)(ScriptBuffer* Buffer, const char* format, ...);
const _EditorLog EditorLog = reinterpret_cast<_EditorLog>(0x5C5730);

void DoSendHotReloadData(Script* script)
{
	const auto* activeFile = DataHandler::Get()->activeFile;
	SocketClient client("127.0.0.1", g_nvsePort);
	ScriptTransferObject scriptTransferObject;
	scriptTransferObject.scriptRefID = script->refID & 0x00FFFFFF;
	scriptTransferObject.dataLength = script->info.dataLength;
	scriptTransferObject.nameLength = strlen(activeFile->name);
	scriptTransferObject.numVars = script->GetVarCount();
	scriptTransferObject.numRefs = script->GetRefCount();
	scriptTransferObject.type = script->info.type;
	client.SendData(scriptTransferObject);
	client.SendData(activeFile->name, scriptTransferObject.nameLength);
	client.SendData(static_cast<char*>(script->data), script->info.dataLength);
	auto* varNode = &script->varList;
	while (varNode)
	{
		if (varNode->data)
		{
			VarInfoTransferObject obj(varNode->data->idx, varNode->data->type, varNode->data->name.m_dataLen);
			client.SendData(obj);
			client.SendData(varNode->data->name.CStr(), varNode->data->name.m_dataLen);
		}
		varNode = varNode->Next();
	}
	auto* refNode = &script->refList;
	while (refNode)
	{
		auto* data = refNode->var;
		if (data)
		{
			const auto* esmName = data->form ? data->form->mods.Head()->data ? data->form->mods.Head()->data->name : nullptr : nullptr;
			RefInfoTransferObject refObj(data->name.m_dataLen, data->form ? data->form->refID & 0x00FFFFFF : 0, esmName ? strlen(esmName) : 0, data->varIdx);
			client.SendData(refObj);
			client.SendData(data->name.CStr(), refNode->var->name.m_dataLen);
			if (esmName)
				client.SendData(esmName, strlen(esmName));
		}
		refNode = refNode->Next();
	}
}

void FreeScriptBuffer(ScriptBuffer* buffer)
{
	FormHeap_Free(buffer->scriptName.m_data);
	FormHeap_Free(buffer);
}

void SendHotReloadData(Script* script, ScriptBuffer* buffer)
{
	try
	{
		// ShowCompilerError(buffer, "Attempting hot reload");
		DoSendHotReloadData(script);
		// ShowCompilerError(buffer, "Hot reload succeeded");
	}
	catch (const SocketException& e)
	{
		_MESSAGE("Hot reload error: %s", e.what());
		if (e.m_errno != 10061) // game isn't open
			EditorLog(buffer, "Hot reload error: %s", e.what());
	}
	FreeScriptBuffer(buffer);
}

std::thread g_hotReloadClientThread;

ScriptBuffer* CopyScriptBuffer(ScriptBuffer* buffer)
{
	auto* copy = static_cast<ScriptBuffer*>(FormHeap_Allocate(sizeof(ScriptBuffer))); // script buffer gets destroyed immediately
	*copy = *buffer;
	copy->scriptName = String();
	copy->scriptName.Set(buffer->scriptName.CStr());
	copy->curLineNumber = 0;
	return copy;
}


void __fastcall SendHotReloadDataHook(Script* script, ScriptBuffer* buffer)
{
	g_hotReloadClientThread = std::thread(SendHotReloadData, script, CopyScriptBuffer(buffer));
	g_hotReloadClientThread.detach();
}

__declspec(naked) void Hook_HotReload()
{
	static UInt32 Script__CopyFromScriptBuffer = 0x5C5100;
	static UInt32 returnLocation = 0x5C97E0;
	static Script* script = nullptr;
	static ScriptBuffer* buffer = nullptr;
	__asm
	{
		mov [script], ecx
		mov [buffer], esi
		call Script__CopyFromScriptBuffer
		mov ecx, script
		mov edx, buffer
		call SendHotReloadDataHook
		jmp returnLocation
	}
}

void InitializeHotReloadEditor()
{
	WriteRelJump(0x5C97DB, UInt32(Hook_HotReload));
	// Patch useless micro optimization that prevents ref variable names getting loaded
	PatchMemoryNop(0x5C5150, 6);
}