"""Native integration tests for the Project vocal-edit facade."""

from __future__ import annotations

import base64
import ctypes
import os
import subprocess
import sys
import textwrap

import numpy as np
import pytest

from libsonare._errors import SonareError
from libsonare._ffi_types_vocal_project import (
    SonareProjectVocalEditApplyDesc,
    SonareProjectVocalEditApplyResult,
    SonareProjectVocalEditDependenciesResult,
    SonareProjectVocalEditDependency,
    SonareProjectVocalOriginalSource,
    SonareProjectVocalRehydrateItem,
    SonareProjectVocalRehydrateResult,
)
from libsonare._project import Project
from libsonare._runtime import SonareValueError
from libsonare.vocal_edit import (
    VocalAnalysis,
    VocalNoteEdit,
    VocalSetNoteEdit,
    VocalStateToken,
    VocalTargetMode,
    create_vocal_edit_session,
)
from libsonare.vocal_project import (
    ProjectVocalOriginalSource,
    ProjectVocalRehydrateStatus,
    get_project_vocal_edit_dependencies,
    rehydrate_project_vocal_edits,
)


def _source_and_render() -> tuple[np.ndarray, int, bytes, np.ndarray, object]:
    sample_rate = 16_000
    source = (0.35 * np.sin(2.0 * np.pi * 440.0 * np.arange(8192) / sample_rate)).astype(np.float32)
    analysis = VocalAnalysis(
        frame_origin_sample=37.25,
        samples_per_frame=512.0,
        frame_length_samples=2048,
        f0_hz=np.full(16, 440.0, dtype=np.float32),
        voiced=np.ones(16, dtype=np.uint8),
        algorithm_id="host",
        algorithm_version=1,
    )
    with create_vocal_edit_session(source, sample_rate, analysis=analysis) as session:
        notes, _ = session.notes()
        assert notes
        note = notes[0]
        with session.begin_edit() as draft:
            draft.apply(
                [
                    VocalSetNoteEdit(
                        note.id,
                        VocalNoteEdit(
                            target_mode=VocalTargetMode.CENTER,
                            target_midi=72.0,
                            amount=1.0,
                            speed_ms=0.0,
                            destination_start_sample=note.source_start_sample,
                            destination_length_samples=(
                                note.source_end_sample - note.source_start_sample
                            ),
                        ),
                    )
                ]
            )
            draft.commit()
        rendered = session.render()
        assert float(np.max(np.abs(rendered.samples - source))) > 1e-3
        state = session.export_state()
        source_sha256 = bytes.fromhex(session.analysis().source_sha256)
        token = rendered.token
    return source, sample_rate, source_sha256, rendered.samples, (state, token)


def test_project_vocal_ffi_layout_matches_header() -> None:
    assert ctypes.sizeof(SonareProjectVocalEditApplyDesc) == 176
    assert ctypes.sizeof(SonareProjectVocalEditApplyResult) == 168
    assert ctypes.sizeof(SonareProjectVocalEditDependency) == 224
    assert ctypes.sizeof(SonareProjectVocalEditDependenciesResult) == 24
    assert ctypes.sizeof(SonareProjectVocalOriginalSource) == 40
    assert ctypes.sizeof(SonareProjectVocalRehydrateItem) == 28
    assert ctypes.sizeof(SonareProjectVocalRehydrateResult) == 24


def test_empty_project_dependencies_and_rehydrate_are_owned_results() -> None:
    with Project.create() as project:
        assert get_project_vocal_edit_dependencies(project) == ()
        assert rehydrate_project_vocal_edits(project, ()) == ()


def test_apply_roundtrip_and_rehydrate_after_project_reload() -> None:
    source, sample_rate, source_sha256, rendered, state_and_token = _source_and_render()
    state, token = state_and_token
    with Project.create() as project:
        project.set_sample_rate(float(sample_rate))
        track_id = project.add_track()
        clip_id = project.add_clip(
            track_id,
            0.0,
            1.0,
            audio=source,
            audio_sample_rate=sample_rate,
        )
        clip = project.clip_by_index(0)
        project.set_audio_source_metadata(
            clip.source_id,
            f"sha256:{source_sha256.hex()}",
            "",
        )
        result = project.apply_vocal_edit(
            clip_id=clip_id,
            expected_source_id=clip.source_id,
            expected_source_sample_rate=sample_rate,
            expected_source_sample_count=source.size,
            expected_source_sha256=source_sha256.hex(),
            expected_clip_length_ppq=clip.length_ppq,
            expected_source_offset_ppq=clip.source_offset_ppq,
            rendered_mono=rendered,
            rendered_sample_rate=sample_rate,
            render_token=token,
            sve1=state,
        )
        assert result.clip_id == clip_id
        assert result.take_id == 0
        assert result.original_source_id == clip.source_id
        assert result.derived_source_id != result.original_source_id
        assert result.sidecar_key == f"libsonare.vocal-edit/clip/{clip_id}/take/0"

        dependencies = project.get_vocal_edit_dependencies()
        assert len(dependencies) == 1
        dependency = dependencies[0]
        assert dependency.original_source_id == clip.source_id
        assert dependency.derived_source_id == result.derived_source_id
        assert dependency.original_pcm_available is True
        assert dependency.derived_pcm_available is True
        before_save_bounce = project.bounce(
            total_frames=source.size,
            num_channels=1,
            sample_rate=sample_rate,
        )
        assert before_save_bounce.shape == (source.size, 1)

        serialized = project.to_json_bytes()

    with Project.from_json(serialized) as restored:
        dependencies = restored.get_vocal_edit_dependencies()
        assert len(dependencies) == 1
        dependency = dependencies[0]
        assert dependency.original_pcm_available is False
        assert dependency.derived_pcm_available is False
        serialized_before_rehydrate = restored.to_json_bytes()
        original_sources = (
            ProjectVocalOriginalSource(
                source_id=dependency.original_source_id,
                mono=source,
                sample_rate=sample_rate,
            ),
        )
        with pytest.raises(SonareError) as cancelled:
            rehydrate_project_vocal_edits(
                restored,
                original_sources,
                cancel=lambda: True,
            )
        assert cancelled.value.code == 8
        assert restored.get_vocal_edit_dependencies()[0].derived_pcm_available is False

        def fail_from_probe() -> bool:
            raise RuntimeError("cancel probe failed")

        with pytest.raises(RuntimeError, match="cancel probe failed"):
            restored.rehydrate_vocal_edits(
                original_sources,
                cancel=fail_from_probe,
            )
        assert restored.get_vocal_edit_dependencies()[0].derived_pcm_available is False

        statuses = restored.rehydrate_vocal_edits(original_sources)
        assert len(statuses) == 1
        assert statuses[0].status is ProjectVocalRehydrateStatus.REHYDRATED
        assert statuses[0].derived_source_id == dependency.derived_source_id
        already_ready = restored.rehydrate_vocal_edits(original_sources)
        assert already_ready[0].status is ProjectVocalRehydrateStatus.ALREADY_READY
        assert already_ready[0].derived_source_id == dependency.derived_source_id
        ready = restored.get_vocal_edit_dependencies()[0]
        assert ready.original_pcm_available is True
        assert ready.derived_pcm_available is True
        after_rehydrate_bounce = restored.bounce(
            total_frames=source.size,
            num_channels=1,
            sample_rate=sample_rate,
        )
        np.testing.assert_array_equal(after_rehydrate_bounce, before_save_bounce)
        assert restored.to_json_bytes() == serialized_before_rehydrate


def test_callback_close_guards_survive_in_isolated_native_process() -> None:
    """Callback initiated close is deferred until the outermost native call returns."""

    source, sample_rate, source_sha256, rendered, state_and_token = _source_and_render()
    state, token = state_and_token
    with Project.create() as project:
        project.set_sample_rate(float(sample_rate))
        track_id = project.add_track()
        clip_id = project.add_clip(
            track_id,
            0.0,
            1.0,
            audio=source,
            audio_sample_rate=sample_rate,
        )
        clip = project.clip_by_index(0)
        project.set_audio_source_metadata(
            clip.source_id,
            f"sha256:{source_sha256.hex()}",
            "",
        )
        project.apply_vocal_edit(
            clip_id=clip_id,
            expected_source_id=clip.source_id,
            expected_source_sample_rate=sample_rate,
            expected_source_sample_count=source.size,
            expected_source_sha256=source_sha256.hex(),
            expected_clip_length_ppq=clip.length_ppq,
            expected_source_offset_ppq=clip.source_offset_ppq,
            rendered_mono=rendered,
            rendered_sample_rate=sample_rate,
            render_token=token,
            sve1=state,
        )
        serialized = project.to_json_bytes()
        source_id = clip.source_id

    child = textwrap.dedent(
        """
        import base64
        import os

        import numpy as np

        from libsonare._errors import SonareError
        from libsonare._project import Project
        from libsonare.vocal_edit import VocalAnalysis, create_vocal_edit_session
        from libsonare.vocal_project import ProjectVocalOriginalSource

        source = np.frombuffer(
            base64.b64decode(os.environ["SONARE_TEST_PCM"]), dtype=np.float32
        ).copy()
        sample_rate = 16000

        def expect_closed(call):
            try:
                call()
            except (RuntimeError, SonareError):
                return
            raise AssertionError("closed owner accepted a call")

        analysis = VocalAnalysis(
            frame_origin_sample=37.25,
            samples_per_frame=512.0,
            frame_length_samples=2048,
            f0_hz=np.full(16, 440.0, dtype=np.float32),
            voiced=np.ones(16, dtype=np.uint8),
            algorithm_id="host",
            algorithm_version=1,
        )
        with create_vocal_edit_session(source, sample_rate, analysis=analysis) as session:
            closing = session.capture_render_snapshot()

            def close_snapshot():
                closing.close()
                return False

            assert closing.render(cancel=close_snapshot).samples.size == source.size
            expect_closed(closing.render)

            with session.capture_render_snapshot() as snapshot:
                job = snapshot.begin_render_job()

                def close_job():
                    job.close()
                    return False

                job.next(cancel=close_job)
                expect_closed(job.next)
                assert snapshot.render().samples.size == source.size

        project = Project.from_json(base64.b64decode(os.environ["SONARE_TEST_PROJECT"]))
        original = ProjectVocalOriginalSource(
            source_id=int(os.environ["SONARE_TEST_SOURCE_ID"]),
            mono=source,
            sample_rate=sample_rate,
        )
        try:
            def close_project():
                project.close()
                return False

            statuses = project.rehydrate_vocal_edits((original,), cancel=close_project)
            assert len(statuses) == 1
            expect_closed(project.get_vocal_edit_dependencies)
        finally:
            project.close()
        print("vocal-lifetime-ok")
        """
    )
    env = os.environ.copy()
    env["SONARE_TEST_PCM"] = base64.b64encode(source.tobytes()).decode("ascii")
    env["SONARE_TEST_PROJECT"] = base64.b64encode(serialized).decode("ascii")
    env["SONARE_TEST_SOURCE_ID"] = str(source_id)
    completed = subprocess.run(
        [sys.executable, "-c", child],
        env=env,
        capture_output=True,
        text=True,
        timeout=30,
        check=False,
    )
    assert completed.returncode == 0, completed.stderr or completed.stdout
    assert "vocal-lifetime-ok" in completed.stdout


@pytest.mark.parametrize(
    "digest",
    [
        "sha256:" + "00" * 32,
        "0" * 63,
        "0" * 65,
        "zz" * 32,
    ],
)
def test_apply_rejects_non_hex_digest_text(digest: str) -> None:
    with Project.create() as project, pytest.raises(SonareValueError):
        project.apply_vocal_edit(
            clip_id=1,
            expected_source_id=1,
            expected_source_sample_rate=16000,
            expected_source_sample_count=1,
            expected_source_sha256=digest,
            expected_clip_length_ppq=1.0,
            expected_source_offset_ppq=0.0,
            rendered_mono=[0.0],
            rendered_sample_rate=16000,
            render_token=VocalStateToken(0, 0, 0, 0),
            sve1=b"x",
        )


def test_apply_rejects_raw_digest_bytes() -> None:
    with Project.create() as project, pytest.raises(TypeError):
        project.apply_vocal_edit(
            clip_id=1,
            expected_source_id=1,
            expected_source_sample_rate=16000,
            expected_source_sample_count=1,
            expected_source_sha256=bytes(32),  # type: ignore[arg-type]
            expected_clip_length_ppq=1.0,
            expected_source_offset_ppq=0.0,
            rendered_mono=[0.0],
            rendered_sample_rate=16000,
            render_token=VocalStateToken(0, 0, 0, 0),
            sve1=b"x",
        )


@pytest.mark.parametrize(
    ("clip_length", "offset"),
    [(0.0, 0.0), (-1.0, 0.0), (1.0, -0.5)],
)
def test_apply_range_validation_comes_from_the_c_abi(clip_length: float, offset: float) -> None:
    source, sample_rate, source_sha256, rendered, state_and_token = _source_and_render()
    state, token = state_and_token
    with Project.create() as project:
        project.set_sample_rate(float(sample_rate))
        track_id = project.add_track()
        clip_id = project.add_clip(track_id, 0.0, 1.0, audio=source, audio_sample_rate=sample_rate)
        clip = project.clip_by_index(0)
        with pytest.raises(SonareError):
            project.apply_vocal_edit(
                clip_id=clip_id,
                expected_source_id=clip.source_id,
                expected_source_sample_rate=sample_rate,
                expected_source_sample_count=source.size,
                expected_source_sha256=source_sha256.hex(),
                expected_clip_length_ppq=clip_length,
                expected_source_offset_ppq=offset,
                rendered_mono=rendered,
                rendered_sample_rate=sample_rate,
                render_token=token,
                sve1=state,
            )


def test_apply_digest_is_case_insensitive() -> None:
    from libsonare.vocal_project import _digest

    assert _digest("AB" * 32, "d") == bytes.fromhex("ab" * 32)
