#include <cstring>

#include "trix.h"

// A different user operator (not matching the main trix.cpp table).
static void different_op(Trix *trx) {
    trx->verify_operands(Trix::VerifyInteger);
    // no-op: just pops the integer
    --trx->m_op_ptr;
}

static constexpr Trix::Operator wrong_user_ops[] = {
        {different_op, "completely-different-operator"},
        {     nullptr,                              {}},
};

int main(int argc, char *argv[]) {
    if (argc < 3) {
        std::println(stderr, "usage: {} --small-vm|--wrong-ops <file.img>", argv[0]);
        return 2;
    }

    const char *mode = argv[1];
    const char *img_file = argv[2];

    if (std::strcmp(mode, "--small-vm") == 0) {
        // Construct a Trix with a 256KB VM (== MinVmSize, the smallest the
        // constructor accepts) and thaw a .img frozen from a 1MB VM whose used
        // bytes exceed 256KB, so the thaw fails the capacity check.  (A
        // sub-MinVmSize size here would instead throw from the constructor
        // before startup_image, never exercising the capacity path.)
        Trix::Config cfg{};
        cfg.m_filename = img_file;
        cfg.m_mode = Trix::StartupMode::ImageFile;
        cfg.m_stream_count = 16;

        Trix trx(256 * 1024, cfg);
        // 256KB is below the frozen image's used bytes, so startup_image()'s
        // capacity check rejects the thaw and returns false; interpreter() is
        // never called and the constructor returns normally.  Returning 1
        // signals "thaw correctly rejected" to run_snapshot_tests.sh.
        return 1;
    }

    if (std::strcmp(mode, "--wrong-ops") == 0) {
        // Construct a Trix with a different user operator table.
        // The thaw should fail because user-op CRC doesn't match.
        Trix::Config cfg{};
        cfg.m_useroperators = wrong_user_ops;
        cfg.m_filename = img_file;
        cfg.m_mode = Trix::StartupMode::ImageFile;
        cfg.m_stream_count = 16;

        Trix trx(1024 * 1024, cfg);
        return 1;
    }

    std::println(stderr, "unknown mode: {}", mode);
    return 2;
}
