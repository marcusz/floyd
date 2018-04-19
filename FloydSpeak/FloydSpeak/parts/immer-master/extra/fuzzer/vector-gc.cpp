//
// immer - immutable data structures for C++
// Copyright (C) 2016, 2017 Juan Pedro Bolivar Puente
//
// This file is part of immer.
//
// immer is free software: you can redistribute it and/or modify
// it under the terms of the GNU Lesser General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// immer is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU Lesser General Public License for more details.
//
// You should have received a copy of the GNU Lesser General Public License
// along with immer.  If not, see <http://www.gnu.org/licenses/>.
//

#include "fuzzer_input.hpp"
#include <immer/vector.hpp>
#include <immer/vector_transient.hpp>
#include <immer/heap/gc_heap.hpp>
#include <immer/refcount/no_refcount_policy.hpp>
#include <iostream>
#include <array>

using gc_memory = immer::memory_policy<
    immer::heap_policy<immer::gc_heap>,
    immer::no_refcount_policy,
    immer::gc_transience_policy,
    false>;

extern "C"
int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    constexpr auto var_count = 4;
    constexpr auto bits      = 2;

    using vector_t    = immer::vector<int, gc_memory, bits, bits>;
    using transient_t = typename vector_t::transient_type;
    using size_t   = std::uint8_t;

    auto vs = std::array<vector_t, var_count>{};
    auto ts = std::array<transient_t, var_count>{};

    auto is_valid_var = [&] (auto idx) {
        return idx >= 0 && idx < var_count;
    };
    auto is_valid_index = [] (auto& v) {
        return [&] (auto idx) { return idx >= 0 && idx < v.size(); };
    };
    auto is_valid_size = [] (auto& v) {
        return [&] (auto idx) { return idx >= 0 && idx <= v.size(); };
    };

    return fuzzer_input{data, size}.run([&] (auto& in)
    {
        enum ops {
            op_transient,
            op_persistent,
            op_push_back,
            op_update,
            op_take,
            op_push_back_mut,
            op_update_mut,
            op_take_mut,
        };
        auto dst = read<char>(in, is_valid_var);
        switch (read<char>(in))
        {
        case op_transient: {
            auto src = read<char>(in, is_valid_var);
            ts[dst] = vs[src].transient();
            break;
        }
        case op_persistent: {
            auto src = read<char>(in, is_valid_var);
            vs[dst] = ts[src].persistent();
            break;
        }
        case op_push_back: {
            auto src = read<char>(in, is_valid_var);
            vs[dst] = vs[src].push_back(42);
            break;
        }
        case op_update: {
            auto src = read<char>(in, is_valid_var);
            auto idx = read<size_t>(in, is_valid_index(vs[src]));
            vs[dst] = vs[src].update(idx, [] (auto x) { return x + 1; });
            break;
        }
        case op_take: {
            auto src = read<char>(in, is_valid_var);
            auto idx = read<size_t>(in, is_valid_size(vs[src]));
            vs[dst] = vs[src].take(idx);
            break;
        }
        case op_push_back_mut: {
            ts[dst].push_back(13);
            break;
        }
        case op_update_mut: {
            auto idx = read<size_t>(in, is_valid_index(ts[dst]));
            ts[dst].update(idx, [] (auto x) { return x + 1; });
            break;
        }
        case op_take_mut: {
            auto idx = read<size_t>(in, is_valid_size(ts[dst]));
            ts[dst].take(idx);
            break;
        }
        default:
            break;
        };
        return true;
    });
}