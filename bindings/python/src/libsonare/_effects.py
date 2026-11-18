"""Public audio-effects facade.

The implementation lives in focused editing, separation, realtime-voice, level,
dynamics and per-family repair modules.  Re-exporting here preserves the
established public API.
"""

from ._effects_dynamics import *  # noqa: F403
from ._effects_dynamics import (
    _coerce_compressor_detector as _coerce_compressor_detector,
)
from ._effects_dynamics import (
    _run_dynamics as _run_dynamics,
)
from ._effects_level import *  # noqa: F403
from ._effects_note_model import *  # noqa: F403
from ._effects_note_ops import *  # noqa: F403
from ._effects_percussive import *  # noqa: F403
from ._effects_repair_common import (
    _run_repair as _run_repair,
)
from ._effects_repair_dereverb import *  # noqa: F403
from ._effects_repair_impulsive import *  # noqa: F403
from ._effects_repair_impulsive import (
    _coerce_decrackle_mode as _coerce_decrackle_mode,
)
from ._effects_repair_noise import *  # noqa: F403
from ._effects_repair_noise import (
    _coerce_denoise_estimator as _coerce_denoise_estimator,
)
from ._effects_repair_noise import (
    _coerce_denoise_mode as _coerce_denoise_mode,
)
from ._effects_repair_session import *  # noqa: F403
from ._effects_repair_trim import *  # noqa: F403
from ._effects_repair_trim import (
    _coerce_trim_silence_mode as _coerce_trim_silence_mode,
)
from ._effects_separation import *  # noqa: F403
from ._effects_spectral import *  # noqa: F403
from ._effects_timepitch import *  # noqa: F403
from ._effects_voice import *  # noqa: F403
from ._effects_voice import (
    _resolve_preset_ordinal as _resolve_preset_ordinal,
)
from ._effects_voice import (
    _voice_config_to_json as _voice_config_to_json,
)
from ._facade import rebind_facade_exports as _rebind_facade_exports

_rebind_facade_exports(globals(), "libsonare._effects_")
del _rebind_facade_exports
