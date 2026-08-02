"""Negative mypy fixture: both lines below must be rejected."""

import libsonare

entry: libsonare.MasteringProcessorCatalogEntry = {
    "id": "dynamics.compressor",
    "kind": "streaming",  # invalid literal
    "realtimeInsertable": True,
    "stereoOnly": False,
    "latencySamples": 0,
    "tailSamples": 0,
    "channelPolicy": "multichannel",
    "category": "dynamics",
    "params": [],
}

typo = entry["latencySample"]  # unknown TypedDict key
