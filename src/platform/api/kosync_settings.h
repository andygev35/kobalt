#pragma once
#include <QString>
#include <functional>

namespace kobalt {
// The kosync login as the settings screen sees it. The host owns the file the
// login lives in; the reader only edits and tests it. Optional: a host without
// this service shows no kosync settings.
struct KosyncSettingsService {
    struct Stored {
        QString server, username;
        // The password itself is never returned.
        bool hasPassword = false;
    };
    enum class Outcome {
        Success,
        Invalid,     // the address, username or password cannot be used as typed
        BadLogin,    // the server answered and refused the username or password
        NotKosync,   // the address answered, but not as a kosync server
        Unreachable, // no answer: offline, a wrong address, or a failed secure connection
        Rejected     // any other answer
    };
    struct TestResult {
        Outcome outcome = Outcome::Unreachable;
        // The network layer's own words for an unreachable server, if it gave any.
        QString detail;
    };
    std::function<Stored()> load;
    // An empty password keeps the stored one. Returns false with `error` set
    // when the login is unusable or cannot be saved.
    std::function<bool(const QString &server, const QString &username, const QString &password, QString *error)> save;
    // Reports once, on the calling thread, including after a timeout.
    std::function<void(const QString &server, const QString &username, const QString &password,
                       std::function<void(TestResult)> done)>
        test;
    bool complete() const {
        return load && save && test;
    }
};
}
