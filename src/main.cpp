#include "_config.hpp"

#include "scotland2/shared/modloader.h"

#include "paper2_scotland2/shared/logger.hpp"

#include "beatsaber-hook/shared/utils/hooking.hpp"

#include "Oculus/Platform/CAPI.hpp"
#include "Oculus/Platform/AccountAgeCategory.hpp"
#include "Oculus/Platform/Message.hpp"

#include <limits>

inline constexpr auto Logger = Paper::ConstLoggerContext("JusticeForQuest");

static modloader::ModInfo modInfo{MOD_ID, VERSION, 0};

using MessageType = Oculus::Platform::Message::MessageType;

struct RequestDescriptor {
	uint64_t requestId;
	MessageType messageType;
	// just some arbitrary address to pass back to C# API
	System::IntPtr nativePtr;
	// if true then PopMessage should skip it as it has already been consumed
	bool popped;
};

template <>
struct fmt::formatter<RequestDescriptor> : fmt::formatter<string_view>
{
	template <typename FormatContext>
    auto format(const RequestDescriptor& d, FormatContext& ctx) const
	{
		return formatter<string_view>::format(fmt::format("(requestId: {}, messageType: {}, nativePtr {})", d.requestId, (uint32_t)d.messageType.value__, d.nativePtr.m_value.convert()), ctx);
	}
};

std::vector<RequestDescriptor> requests;

uint64_t getNextId() {
	static uint64_t currentId = std::numeric_limits<uint64_t>::max();
	return currentId--;
}

MAKE_HOOK_MATCH(CAPI_ovr_UserAgeCategory_Get, &Oculus::Platform::CAPI::ovr_UserAgeCategory_Get, uint64_t)
{
	uint64_t requestId = getNextId();
	Logger.debug("Returning custom OVR message ID for ovr_UserAgeCategory_Get: {}", requestId);

	requests.emplace_back(requestId, MessageType::UserAgeCategory_Get, System::IntPtr(cordl_internals::Ptr<void>(new uint8_t)));
	return requestId;
}

MAKE_HOOK_MATCH(CAPI_ovr_PopMessage, &Oculus::Platform::CAPI::ovr_PopMessage, System::IntPtr)
{
	if(requests.size() > 0) {
		auto request = std::ranges::find_if(requests, [](const RequestDescriptor& descriptor) 
			{ return descriptor.popped == false; });
		if(request != requests.end()) {
			Logger.debug("Popping custom message {}", *request);
			request->popped = true;
			// freeing occurs later via a FreeMessage call from C#
			return request->nativePtr;
		}
	}

	return CAPI_ovr_PopMessage();
}

MAKE_HOOK_MATCH(CAPI_ovr_Message_GetType, &Oculus::Platform::CAPI::ovr_Message_GetType, MessageType, System::IntPtr ptr)
{
	auto iter = std::ranges::find_if(requests, [ptr](const RequestDescriptor& descriptor)
		{ return descriptor.nativePtr.m_value.convert() == ptr.m_value.convert(); });
	if(iter != requests.end())
	{
		Logger.debug("Returning custom message type for message {}", *iter);
		return iter->messageType;
	}

	return CAPI_ovr_Message_GetType(ptr);
}

MAKE_HOOK_MATCH(CAPI_ovr_Message_GetNativeMessage, &Oculus::Platform::CAPI::ovr_Message_GetNativeMessage, System::IntPtr, System::IntPtr obj)
{
	auto iter = std::ranges::find_if(requests, [obj](const RequestDescriptor& descriptor)
		{ return descriptor.nativePtr.m_value.convert() == obj.m_value.convert(); });
	if(iter != requests.end())
	{
		// obj and nativePtr are the same pointer since we dont have any reason to have multiple since we are only proxying the methods
		return obj;
	}

	return CAPI_ovr_Message_GetNativeMessage(obj);
}

MAKE_HOOK_MATCH(CAPI_ovr_Message_GetUserAccountAgeCategory, &Oculus::Platform::CAPI::ovr_Message_GetUserAccountAgeCategory, System::IntPtr, System::IntPtr obj)
{
	auto iter = std::ranges::find_if(requests, [obj](const RequestDescriptor& descriptor)
		{ return descriptor.nativePtr.m_value.convert() == obj.m_value.convert(); });
	if(iter != requests.end())
	{
		return iter->nativePtr;
	}

	return CAPI_ovr_Message_GetUserAccountAgeCategory(obj);
}

MAKE_HOOK_MATCH(CAPI_ovr_FreeMessage, &Oculus::Platform::CAPI::ovr_FreeMessage, void, System::IntPtr obj)
{
	auto iter = std::ranges::find_if(requests, [obj](const RequestDescriptor& descriptor)
		{ return descriptor.nativePtr.m_value.convert() == obj.m_value.convert(); });
	if(iter != requests.end())
	{
		Logger.debug("Freeing custom message {}", *iter);
		delete reinterpret_cast<uint8_t*>(iter->nativePtr.m_value.convert());
		requests.erase(iter);
		return;
	}

	CAPI_ovr_FreeMessage(obj);
}

MAKE_HOOK_MATCH(CAPI_ovr_UserAccountAgeCategory_GetAgeCategory, &Oculus::Platform::CAPI::ovr_UserAccountAgeCategory_GetAgeCategory, Oculus::Platform::AccountAgeCategory, System::IntPtr obj)
{
	auto iter = std::ranges::find_if(requests, [obj](const RequestDescriptor& descriptor)
		{ return descriptor.nativePtr.m_value.convert() == obj.m_value.convert(); });
	if(iter != requests.end())
	{
		Logger.debug("Returning AccountAgeCategory::Ad for custom message id: {}", iter->requestId);
		return Oculus::Platform::AccountAgeCategory::Ad;
	}

	return CAPI_ovr_UserAccountAgeCategory_GetAgeCategory(obj);
}

MAKE_HOOK_MATCH(CAPI_ovr_Message_IsError, &Oculus::Platform::CAPI::ovr_Message_IsError, bool, System::IntPtr obj)
{
	auto iter = std::ranges::find_if(requests, [obj](const RequestDescriptor& descriptor)
		{ return descriptor.nativePtr.m_value.convert() == obj.m_value.convert(); });
	if(iter != requests.end())
	{
		return false;
	}

	return CAPI_ovr_Message_IsError(obj);
}

MAKE_HOOK_MATCH(CAPI_Message_GetRequestID, &Oculus::Platform::CAPI::ovr_Message_GetRequestID, uint64_t, System::IntPtr obj)
{
	auto iter = std::ranges::find_if(requests, [obj](const RequestDescriptor& descriptor)
		{ return descriptor.nativePtr.m_value.convert() == obj.m_value.convert(); });
	if(iter != requests.end())
	{
		return iter->requestId;
	}

	return CAPI_Message_GetRequestID(obj);
}

MOD_EXTERN_FUNC void setup(CModInfo *info) noexcept {
  *info = modInfo.to_c();

  // File logging
  Paper::Logger::RegisterFileContextId(Logger.tag);

  Logger.info("Completed setup!");
}

MOD_EXTERN_FUNC void load() noexcept {
	il2cpp_functions::Init();

	Logger.info("Installing hooks...");

	INSTALL_HOOK(Logger, CAPI_ovr_UserAgeCategory_Get);
	INSTALL_HOOK(Logger, CAPI_ovr_PopMessage);
	INSTALL_HOOK(Logger, CAPI_ovr_Message_GetType);
	INSTALL_HOOK(Logger, CAPI_ovr_Message_GetNativeMessage);
	INSTALL_HOOK(Logger, CAPI_ovr_Message_GetUserAccountAgeCategory);
	INSTALL_HOOK(Logger, CAPI_ovr_UserAccountAgeCategory_GetAgeCategory);
	INSTALL_HOOK(Logger, CAPI_ovr_Message_IsError);
	INSTALL_HOOK(Logger, CAPI_Message_GetRequestID);
	INSTALL_HOOK(Logger, CAPI_ovr_FreeMessage);

	Logger.info("Installed all hooks!");
}