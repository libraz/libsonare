"""Public feature-extraction facade.

Implementations are grouped by DSP responsibility so this stable import path
does not grow with every new feature family.
"""

from ._facade import rebind_facade_exports as _rebind_facade_exports
from ._features_core import *  # noqa: F403
from ._features_core import _chroma_variant as _chroma_variant
from ._features_metering import *  # noqa: F403
from ._features_metering import (
    _metering_scalar as _metering_scalar,
)
from ._features_metering import (
    _scale_scalar as _scale_scalar,
)
from ._features_metering import (
    _stereo_scalar as _stereo_scalar,
)
from ._features_metering import (
    _waveform_peaks_from_c as _waveform_peaks_from_c,
)
from ._features_transforms import *  # noqa: F403
from ._features_transforms import (
    _cqt_result_from_c as _cqt_result_from_c,
)
from ._features_transforms import (
    _cqt_variant as _cqt_variant,
)

_rebind_facade_exports(globals(), "libsonare._features_")
del _rebind_facade_exports
