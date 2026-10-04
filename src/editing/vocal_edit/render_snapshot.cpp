// SONARE_WASM_EXCEPTION_UNWIND: release captured state and source ownership on allocation failure.
#include "editing/vocal_edit/render_snapshot.h"

// The snapshot is an immutable value handle. Its ownership and accessors are
// defined inline in the header so the session and renderer can share the same
// lightweight handle without introducing a mutable dependency between them.
