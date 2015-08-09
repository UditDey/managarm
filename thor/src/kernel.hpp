
#include "../../frigg/include/types.hpp"
#include "../../frigg/include/traits.hpp"
#include "../../frigg/include/support.hpp"
#include "../../frigg/include/debug.hpp"
#include "../../frigg/include/algorithm.hpp"
#include "../../frigg/include/initializer.hpp"
#include "../../frigg/include/memory.hpp"
#include "../../frigg/include/vector.hpp"
#include "../../frigg/include/hashmap.hpp"
#include "../../frigg/include/linked.hpp"
#include "../../frigg/include/variant.hpp"
#include "../../frigg/include/libc.hpp"

#include "runtime.hpp"
#include "util/smart-ptr.hpp"
#include "physical.hpp"
#include "paging.hpp"
#include "core.hpp"
#include "schedule.hpp"

namespace thor {
namespace k_init {

void main();

} } // namespace thor::k_init
