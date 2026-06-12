#include "trix.h"

#include <chrono>
#include <thread>
#include <vector>

// -----------------------------------------------------------------------
// Example user operators -- demonstrates the public operator API.
// -----------------------------------------------------------------------

// Background interrupt workers spawned by my-raise-interrupt.  Host-owned and
// joined in main() before the Trix object is destroyed, so a worker can never
// lock the destroyed m_mutex or touch the freed VM heap after the script ends.
static std::vector<std::thread> g_interrupt_workers;

// Integer :- Integer
// Computes the square of the top integer; raises NumericalOverflow on overflow.
static void my_square_op(Trix *trx) {
    trx->verify_operands(Trix::VerifyInteger);

    auto val = trx->m_op_ptr->integer_value();
    Trix::integer_t result;
    if (__builtin_smul_overflow(val, val, &result)) {
        trx->error(Trix::Error::NumericalOverflow, "'my-square': {0} * {0} overflows Integer", val);
    } else {
        *trx->m_op_ptr = Trix::Object::make_integer(result);
    }
}

// Integer Integer Integer :- Integer
// Stack order: value min max my-clamp :- clamped-value
// Clamps value to [min, max]; raises RangeCheck if min > max.
static void my_clamp_op(Trix *trx) {
    trx->verify_operands(Trix::VerifyInteger, Trix::VerifyInteger, Trix::VerifyInteger);

    auto max_val = trx->m_op_ptr->integer_value();
    --trx->m_op_ptr;
    auto min_val = trx->m_op_ptr->integer_value();
    --trx->m_op_ptr;
    auto val = trx->m_op_ptr->integer_value();
    if (min_val > max_val) {
        trx->error(Trix::Error::RangeCheck, "'my-clamp': min {} > max {}", min_val, max_val);
    } else {
        auto result = (val < min_val) ? min_val : (val > max_val) ? max_val : val;
        *trx->m_op_ptr = Trix::Object::make_integer(result);
    }
}

// Integer :- --
// Raises an interrupt at the given level (0, 1, or 2) from a background thread
// after a brief delay.  The interrupt is delivered asynchronously, exercising
// the full raise_interrupt -> process_interrupt -> exec stack dispatch path.
// Level 3 raises ExitIRQ -- the host-side stop signal a parked --resident
// instance wakes on (see Stream::init's resident floor handling).
static void my_raise_interrupt_op(Trix *trx) {
    trx->verify_operands(Trix::VerifyInteger);

    auto level = trx->m_op_ptr->integer_value();
    --trx->m_op_ptr;

    Trix::interrupt_t irq;
    switch (level) {
    case 0:
        irq = Trix::Level0IRQ;
        break;

    case 1:
        irq = Trix::Level1IRQ;
        break;

    case 2:
        irq = Trix::Level2IRQ;
        break;

    case 3:
        irq = Trix::ExitIRQ;
        break;

    default:
        trx->error(Trix::Error::RangeCheck, "'my-raise-interrupt': level must be 0, 1, 2, or 3 (3 = exit)");
    }

    g_interrupt_workers.emplace_back([trx, irq]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        static_cast<void>(trx->raise_interrupt(irq));
    });
}

// Null-terminated user operator table.
static constexpr Trix::Operator user_ops[] = {
        {         my_square_op,          "my-square"},
        {          my_clamp_op,           "my-clamp"},
        {my_raise_interrupt_op, "my-raise-interrupt"},
        {              nullptr,                   {}},
};

int main(int argc, char *argv[]) {
    auto result = Trix::parse_args(argc, argv);
    if (result.should_exit) {
        return result.exit_code;
    } else {
        result.config.m_useroperators = user_ops;
        // Only override when the user left the library default, so an explicit
        // --stream-count=N is honored (parse_args has already applied it).
        if (result.config.m_stream_count == Trix::DefaultStreamCount) {
            result.config.m_stream_count = 16;
        }

        // NOT const: a my-raise-interrupt worker thread holds a Trix* captured
        // inside the constructor and calls the mutating raise_interrupt() on it.
        // NOLINTNEXTLINE(misc-const-correctness) -- cross-thread mutation clang-tidy can't see.
        Trix trx(result.vm_size, result.config);
        // Drain background interrupt workers while `trx` is still alive, so no
        // worker can lock the destroyed m_mutex or touch the freed VM heap.
        for (auto &worker : g_interrupt_workers) {
            worker.join();
        }
        return trx.exit_code();
    }
}
