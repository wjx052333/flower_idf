#pragma once

/* Stub Assets singleton. */
class Assets {
public:
    static Assets& GetInstance() {
        static Assets instance;
        return instance;
    }

    bool IsValid() const { return false; }
};