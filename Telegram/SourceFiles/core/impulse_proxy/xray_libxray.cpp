#include "core/impulse_proxy/xray_libxray.h"

#ifdef _WIN32

#include "base/platform/win/base_windows_safe_library.h"

#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace Impulse {

struct XrayLibrary::Impl {
	using InvokeFunction = char *(*)(char *);
	using FreeFunction = void (*)(char *);

	HINSTANCE module = nullptr;
	InvokeFunction invokeCall = nullptr;
	FreeFunction freeCall = nullptr;

	std::thread worker;
	std::mutex mutex;
	std::condition_variable condition;
	std::deque<std::function<void()>> tasks;
	bool stopping = false;

	~Impl() {
		{
			std::lock_guard<std::mutex> lock(mutex);
			stopping = true;
		}
		condition.notify_all();
		if (worker.joinable()) {
			worker.join();
		}
	}

	void post(std::function<void()> task) {
		{
			std::lock_guard<std::mutex> lock(mutex);
			tasks.push_back(std::move(task));
		}
		condition.notify_one();
	}

	// Runs queued tasks in order. On shutdown the queue is drained first, so
	// every future handed out is fulfilled before the DLL calls stop.
	void run() {
		while (true) {
			std::function<void()> task;
			{
				std::unique_lock<std::mutex> lock(mutex);
				condition.wait(lock, [this]() {
					return stopping || !tasks.empty();
				});
				if (tasks.empty()) {
					return;
				}
				task = std::move(tasks.front());
				tasks.pop_front();
			}
			task();
		}
	}

	[[nodiscard]] std::string callLibrary(const std::string &request) {
		std::vector<char> buffer(request.begin(), request.end());
		buffer.push_back('\0');

		char *const response = invokeCall(buffer.data());
		if (!response) {
			return std::string();
		}
		std::string result(response);
		freeCall(response);
		return result;
	}
};

XrayLibrary::XrayLibrary(const std::wstring &dllPath)
: impl(std::make_unique<Impl>()) {
	impl->module = ::LoadLibraryExW(
		dllPath.c_str(),
		nullptr,
		LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR);
	if (!impl->module) {
		return;
	}

	const bool resolved = base::Platform::LoadMethod(
			impl->module,
			"CGoInvoke",
			impl->invokeCall)
		&& base::Platform::LoadMethod(
			impl->module,
			"CGoFree",
			impl->freeCall);
	if (!resolved) {
		impl->module = nullptr;
		impl->invokeCall = nullptr;
		impl->freeCall = nullptr;
		return;
	}

	impl->worker = std::thread([raw = impl.get()]() {
		raw->run();
	});
}

XrayLibrary::~XrayLibrary() = default;

bool XrayLibrary::loaded() const {
	return impl->module != nullptr;
}

std::future<std::string> XrayLibrary::invoke(
		const std::string &method,
		const std::string &payloadJson) {
	std::shared_ptr<std::promise<std::string>> promise
		= std::make_shared<std::promise<std::string>>();
	std::future<std::string> future = promise->get_future();
	if (!loaded()) {
		promise->set_value(std::string());
		return future;
	}

	const std::string request = "{\"apiVersion\":3,\"method\":\""
		+ method
		+ "\",\"payload\":"
		+ payloadJson
		+ "}";
	Impl *const raw = impl.get();
	impl->post([raw, promise, request]() {
		promise->set_value(raw->callLibrary(request));
	});
	return future;
}

} // namespace Impulse

#endif // _WIN32
