#pragma once

#include <algorithm>
#include <cstdint>
#include <optional>
#include <vector>

namespace Flux {
    // Places dans un tableau de `capacity` éléments (sommets ou indices d'une page de géométrie) : la première place
    // libre assez grande, rendue par Free et réunie à ses voisines libres. Peu de places (une par modèle chargé) :
    // une liste triée suffit.
    class RangeAllocator {
        public:
        explicit RangeAllocator(std::uint32_t capacity = 0) {
            if (capacity > 0) {
                m_free.push_back({0, capacity});
            }
        }

        // Début de la place réservée, ou rien si aucune place libre n'est assez grande.
        std::optional<std::uint32_t> Allocate(std::uint32_t size) {
            if (size == 0) {
                return 0u;
            }
            for (auto range = m_free.begin(); range != m_free.end(); ++range) {
                if (range->size >= size) {
                    const std::uint32_t offset = range->offset;
                    range->offset += size;
                    range->size -= size;
                    if (range->size == 0) {
                        m_free.erase(range);
                    }
                    return offset;
                }
            }
            return std::nullopt;
        }

        void Free(std::uint32_t offset, std::uint32_t size) {
            if (size == 0) {
                return;
            }
            const auto next = std::ranges::lower_bound(m_free, offset, {}, &Range::offset);
            const auto inserted = m_free.insert(next, {offset, size});
            // Réunie à la suivante, puis à la précédente si elles se touchent.
            if (const auto after = inserted + 1; after != m_free.end() && inserted->offset + inserted->size == after->offset) {
                inserted->size += after->size;
                m_free.erase(after);
            }
            if (inserted != m_free.begin()) {
                if (const auto before = inserted - 1; before->offset + before->size == inserted->offset) {
                    before->size += inserted->size;
                    m_free.erase(inserted);
                }
            }
        }

        // Plus grande place libre (aucune : 0).
        std::uint32_t GetLargestFree() const {
            std::uint32_t largest = 0;
            for (const Range& range : m_free) {
                largest = std::max(largest, range.size);
            }
            return largest;
        }

        private:
        struct Range
        {
            std::uint32_t offset;
            std::uint32_t size;
        };
        std::vector<Range> m_free; // triées par début, jamais deux qui se touchent
    };
}
