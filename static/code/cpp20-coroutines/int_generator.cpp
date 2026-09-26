#include <coroutine>
#include <exception>
#include <iostream>
#include <optional>
#include <utility>

class IntGenerator {
public:
    struct promise_type;
    using Handle = std::coroutine_handle<promise_type>;

    struct promise_type {
        int current_value{};
        std::exception_ptr error;

        IntGenerator get_return_object() noexcept {
            return IntGenerator{Handle::from_promise(*this)};
        }
        std::suspend_always initial_suspend() const noexcept { return {}; }
        std::suspend_always final_suspend() const noexcept { return {}; }
        std::suspend_always yield_value(int value) noexcept {
            current_value = value;
            return {};
        }
        void return_void() noexcept {}
        void unhandled_exception() noexcept {
            error = std::current_exception();
        }
        void await_transform() = delete;
    };

    IntGenerator(const IntGenerator&) = delete;
    IntGenerator& operator=(const IntGenerator&) = delete;
    IntGenerator& operator=(IntGenerator&&) = delete;

    IntGenerator(IntGenerator&& other) noexcept
        : handle_(std::exchange(other.handle_, {})) {}

    ~IntGenerator() {
        if (handle_) {
            handle_.destroy();
        }
    }

    std::optional<int> next() {
        if (!handle_ || handle_.done()) {
            return std::nullopt;
        }
        handle_.resume();
        auto& promise = handle_.promise();
        if (promise.error) {
            std::rethrow_exception(promise.error);
        }
        if (handle_.done()) {
            return std::nullopt;
        }
        return promise.current_value;
    }

private:
    explicit IntGenerator(Handle handle) noexcept : handle_(handle) {}
    Handle handle_;
};

IntGenerator count(int limit) {
    for (int value = 0; value < limit; ++value) {
        co_yield value;
    }
    co_return;
}

int main() {
    auto numbers = count(3);
    while (auto value = numbers.next()) {
        std::cout << *value << '\n';
    }
    return 0;
}
