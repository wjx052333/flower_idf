#pragma once

#include <string>

// Minimal Assets stub for flower — the custom_wake_word uses Assets::GetAssetData
// to load index.json from the assets partition. On flower, there is no assets
// partition, so GetAssetData always returns false and the defaults are used.

class Assets {
public:
    static Assets& GetInstance() {
        static Assets instance;
        return instance;
    }

    bool GetAssetData(const std::string& name, void*& ptr, size_t& size) {
        return false;
    }

private:
    Assets() = default;
};