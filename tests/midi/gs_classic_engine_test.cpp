// clang-format off
/// @file gs_classic_engine_test.cpp
/// @brief The GS classic graph engine against the soundings renderer, node by node.
///
/// Every k* array below was generated once by the snippet in this comment, run with
/// the soundings archive's own venv (`../soundings/.venv/bin/python`). Graph nodes go
/// through `soundings.render.graph.run` and maps through `soundings.reproduce._from_map`;
/// x-noise has no soundings counterpart, so its rows are the engine's definition
/// restated in numpy. Pan laws and control curves are stored as float, so those rows
/// carry a float-rounding tolerance; everything else is drawn with the same double
/// operations in the same order and compares to 1e-12 or exactly.
///
/// @code{.py}
/// import numpy as np
/// from soundings import reproduce
/// from soundings.render import graph
///
/// N = 24
/// FS = 32000.0
/// IN_L = np.array([((n * 37) % 17 - 8) / 8.0 for n in range(N)])
/// IN_R = np.array([((n * 11) % 13 - 6) / 6.0 for n in range(N)])
/// C = lambda v: {"value": v}
/// B = lambda slot, m: {"byte": f"s{slot}", "map": m}
/// K = lambda ref, m: {"control": ref, "map": m}
/// LAW = {"sides": {"left": [[i, (127 - i) / 127] for i in range(128)],
///                  "right": [[i, i / 127] for i in range(128)]}}
///
/// def run(nodes, out_l, out_r, bytes_now=None):
///     model = {"model": {"id": "t"}, "sample_rate_hz": 32000, "inputs": ["in_l", "in_r"],
///              "outputs": {"out_l": out_l, "out_r": out_r}, "nodes": nodes, "referenced": {"law": LAW}}
///     got = graph.run(model, {"in_l": IN_L, "in_r": IN_R}, bytes_now or {})
///     return got["out_l"], got["out_r"]
///
/// def show(name, arr):
///     print(f"constexpr double {name}[] = {{" + ", ".join(f"{v:.17g}" for v in arr) + "};")
///
/// table = {"kind": "table", "entries": [0.25, 0.5, 0.75, 1.0], "out_of_range": 3}
/// l, _ = run([{"id": "g", "kind": "gain", "input": "in_l", "unit": "ratio", "gain": B(0, table)}], "g", "in_r", {"s0": 2})
/// show("kGain", l)
///
/// nodes = [{"id": f"g{k}", "kind": "gain", "input": "in_l" if k % 2 == 0 else "in_r", "unit": "ratio",
///           "gain": C(1.0 / (k + 1))} for k in range(16)]
/// weights = {f"g{k}": C((k - 7.5) / 8.0) for k in range(16)}
/// weights["g3"] = B(0, {"kind": "window", "low": 10, "high": 50, "at_low": -1.0, "at_high": 3.0})
/// weights["g5"] = C(0.0)
/// nodes.append({"id": "m", "kind": "mix", "inputs": [f"g{k}" for k in range(16)], "weights": weights})
/// l, _ = run(nodes, "m", "in_r", {"s0": 30})
/// show("kMix16", l)
///
/// win = {"kind": "window", "low": 0, "high": 64, "at_low": 0.0, "at_high": 127.0}
/// l, r = run([{"id": "p", "kind": "pan", "input": "in_l", "law": "law", "position": B(0, win)}], "p.left", "p.right", {"s0": 21})
/// show("kPanLeft", l); show("kPanRight", r)
///
/// nodes = [{"id": "l", "kind": "lfo", "shape": "sine", "rate_hz": C(1000.0), "phase_offset": C(0.0)},
///          {"id": "d", "kind": "delay", "input": "in_l", "interpolation": "linear", "time_ms": C(0.1),
///           "modulated_by": {"control": "l", "depth_ms": C(0.05)}},
///          {"id": "d0", "kind": "delay", "input": "in_r", "interpolation": "none", "time_ms": C(0.1)}]
/// l, r = run(nodes, "d", "d0")
/// show("kDelayModulated", l); show("kDelayNone", r)
///
/// for shape in ("sine", "triangle", "square", "saw"):
///     l, _ = run([{"id": "l", "kind": "lfo", "shape": shape, "rate_hz": C(1500.0), "phase_offset": C(0.3)}], "l", "in_r")
///     show("kLfo_" + shape, l)
/// pts = [[0.0, 0.0], [0.25, 1.0], [0.6, -0.5]]
/// l, _ = run([{"id": "l", "kind": "lfo", "shape": "points", "points": pts, "rate_hz": C(2000.0), "phase_offset": C(0.1)}], "l", "in_r")
/// show("kLfoPoints", l)
/// nodes = [{"id": "a", "kind": "lfo", "shape": "sine", "rate_hz": C(700.0), "phase_offset": C(0.0)},
///          {"id": "b", "kind": "lfo", "shape": "saw", "rate_hz": K("a", {"kind": "points", "points": [[-1.0, 200.0], [1.0, 1800.0]]}),
///           "phase_offset": C(0.25)}]
/// l, _ = run(nodes, "b", "in_r")
/// show("kLfoControlRate", l)
///
/// l, _ = run([{"id": "h", "kind": "hold", "input": "in_l", "rate_hz": C(3000.0)}], "h", "in_r")
/// show("kHold", l)
/// l, _ = run([{"id": "g", "kind": "gain", "input": "in_l", "unit": "ratio", "gain": C(0.7)},
///             {"id": "q", "kind": "quantize", "input": "g", "bits": 3}], "q", "in_r")
/// show("kQuantize", l)
///
/// nodes = [{"id": "m", "kind": "mix", "inputs": ["in_l", "g"], "weights": {"in_l": C(1.0), "g": C(1.0)}},
///          {"id": "d", "kind": "delay", "input": "m", "interpolation": "none", "time_ms": C(1000.0 / 32000.0)},
///          {"id": "g", "kind": "gain", "input": "d", "unit": "ratio", "gain": C(0.5)}]
/// l, _ = run(nodes, "m", "in_r")
/// show("kLoop", l)
///
/// # x-noise: the engine's own definition (no soundings counterpart), written in numpy.
/// def xorshift(seed, n):
///     s, out = np.uint32(seed), []
///     for _ in range(n):
///         s ^= np.uint32(s << np.uint32(13)); s ^= np.uint32(s >> np.uint32(17)); s ^= np.uint32(s << np.uint32(5))
///         out.append(int(s))
///     return out
/// def w16(u): return np.int16(np.uint16(u >> 16)) / 32768.0
/// def noise(kind, level, param, aux, n=N):
///     s = np.uint32(0x9E3779B9 ^ aux); out = []; b0 = b1 = b2 = y = ph = 0.0
///     def nxt():
///         nonlocal s
///         s ^= np.uint32(s << np.uint32(13)); s ^= np.uint32(s >> np.uint32(17)); s ^= np.uint32(s << np.uint32(5))
///         return int(s)
///     for i in range(n):
///         if kind == "white": v = w16(nxt())
///         elif kind == "pink":
///             w = w16(nxt())
///             b0 = 0.99765 * b0 + w * 0.0990460; b1 = 0.96300 * b1 + w * 0.2965164; b2 = 0.57000 * b2 + w * 1.0526913
///             v = (b0 + b1 + b2 + w * 0.1848) * 0.336
///         elif kind == "radio":
///             c = np.exp(-2.0 * np.pi * param / FS); y = (1.0 - c) * w16(nxt()) + c * y; v = y
///         elif kind == "disc":
///             u = nxt(); v = w16(nxt()) if (u >> 16) < param * 65536.0 / FS else 0.0
///         else:
///             t = 2.0 * np.pi * ph; v = (np.sin(t) + 0.5 * np.sin(2.0 * t) + 0.25 * np.sin(3.0 * t)) / 1.75
///             ph += param / FS; ph -= np.floor(ph)
///         out.append(level * v)
///     return out
/// show("kNoiseWhite", noise("white", 0.5, 0.0, 7)); show("kNoisePink", noise("pink", 0.5, 0.0, 7))
/// show("kNoiseRadio", noise("radio", 0.5, 3000.0, 7)); show("kNoiseDisc", noise("disc", 0.5, 8000.0, 7))
/// show("kNoiseHum", noise("hum", 0.5, 1000.0, 7))
///
/// specs = {
///     "Stepped": {"kind": "stepped-table", "entries": [3.0, 1.5, 0.1, 7.25, 9.0], "per_entry": 8},
///     "Window": {"kind": "window", "low": 20, "high": 99, "at_low": 0.3, "at_high": -12.7},
///     "States": {"kind": "states", "values": {"0": 0.1, "5": 2.5, "*": -1.0}},
///     "Points": {"kind": "points", "points": [[0, 0.3], [40, 1.7], [127, 0.05]]},
///     "LogPoints": {"kind": "points", "log": True, "points": [[0, 20.0], [64, 700.0], [127, 11000.0]]},
///     "Table": {"kind": "table", "entries": [0.1 * k + 0.05 for k in range(100)], "out_of_range": 7},
/// }
/// for name, spec in specs.items():
///     print(f"constexpr double kMap{name}[] = {{" + ", ".join(f"{reproduce._from_map(spec, b):.17g}" for b in range(128)) + "};")
/// @endcode
// clang-format on

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <utility>
#include <vector>

#include "midi/synth/gs_classic/graph_engine.h"
#include "midi/synth/gs_classic/model_format.h"

namespace {

// clang-format off
constexpr double kGain[] = {-0.75, -0.46875, -0.1875, 0.09375, 0.375, 0.65625, -0.65625, -0.375, -0.09375, 0.1875, 0.46875, 0.75, -0.5625, -0.28125, 0, 0.28125, 0.5625, -0.75, -0.46875, -0.1875, 0.09375, 0.375, 0.65625, -0.65625};
constexpr double kMix16[] = {1.149316221972472, 0.71664078109390617, 0.28646410013597512, -0.14371258082195582, -0.57388926177988664, -1.0040659427378178, 1.0056036411505165, 0.57292820027195024, 0.1427515193140193, -0.28742516164391163, -0.71760184260184257, -1.1477785235597733, 0.86189106032856033, 0.43171437937062934, -0.00096106150793650452, -0.43113774246586756, -0.86131442342379838, 1.1483551604645352, 0.71817847950660452, 0.28800179854867347, -0.14467364232989238, -0.57485032328782326, -1.0050270042457545, 1.0046425796425797};
constexpr double kPanLeft[] = {-0.671875, -0.419921875, -0.16796875, 0.083984375, 0.3359375, 0.587890625, -0.587890625, -0.3359375, -0.083984375, 0.16796875, 0.419921875, 0.671875, -0.50390625, -0.251953125, 0, 0.251953125, 0.50390625, -0.671875, -0.419921875, -0.16796875, 0.083984375, 0.3359375, 0.587890625, -0.587890625};
constexpr double kPanRight[] = {-0.328125, -0.205078125, -0.08203125, 0.041015625, 0.1640625, 0.287109375, -0.287109375, -0.1640625, -0.041015625, 0.08203125, 0.205078125, 0.328125, -0.24609375, -0.123046875, 0, 0.123046875, 0.24609375, -0.328125, -0.205078125, -0.08203125, 0.041015625, 0.1640625, 0.287109375, -0.287109375};
constexpr double kDelayModulated[] = {0, 0, 0, 0, -0.66862915010152335, -0.82388176738152707, -0.50432771950677224, -0.16347116824193852, 0.19999999999999973, 0.58652883175806148, 0.3118626910316038, -0.69888176738152741, -0.24926406871192874, 0.21665786018823863, 0.69538994058094605, 0.1462529016451608, -0.45000000000000007, 0.042054193209677015, 0.5296100594190537, -0.45559665245488501, -0.65073593128807172, -0.20111823261847289, 0.22932771950677186, 0.63847116824193817};
constexpr double kDelayNone[] = {0, 0, 0, -1, 0.83333333333333337, 0.5, 0.16666666666666666, -0.16666666666666666, -0.5, -0.83333333333333337, 1, 0.66666666666666663, 0.33333333333333331, 0, -0.33333333333333331, -0.66666666666666663, -1, 0.83333333333333337, 0.5, 0.16666666666666666, -0.16666666666666666, -0.5, -0.83333333333333337, 1};
constexpr double kLfo_sine[] = {0.95105651629515364, 0.8204014435255137, 0.61909394930983419, 0.36447049987914965, 0.078459095727845068, -0.21430915306505094, -0.48862124149695502, -0.72085359670291882, -0.89100652418836779, -0.9844265680898916, -0.99306845695492629, -0.91618795711713574, -0.76040596560003082, -0.53913832291100017, -0.27144044986507426, 0.019633692460628169, 0.30901699437494717, 0.57178796022761191, 0.78531693088074517, 0.93121493475880368, 0.99691733373312796, 0.97676588132087239, 0.87249600707279673, 0.69308736254563608};
constexpr double kLfo_triangle[] = {0.80000000000000004, 0.61250000000000004, 0.42500000000000004, 0.23750000000000004, 0.050000000000000044, -0.13750000000000018, -0.32500000000000018, -0.51250000000000018, -0.70000000000000018, -0.88750000000000018, -0.92499999999999982, -0.73749999999999982, -0.54999999999999982, -0.36249999999999982, -0.17499999999999982, 0.012500000000000178, 0.20000000000000018, 0.38750000000000018, 0.57500000000000018, 0.76250000000000018, 0.95000000000000018, 0.86249999999999982, 0.67499999999999982, 0.48749999999999982};
constexpr double kLfo_square[] = {1, 1, 1, 1, 1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 1, 1, 1, 1, 1, 1, 1, 1, 1};
constexpr double kLfo_saw[] = {0.60000000000000009, 0.69375000000000009, 0.78750000000000009, 0.88125000000000009, 0.97500000000000009, -0.93124999999999991, -0.83749999999999991, -0.74374999999999991, -0.64999999999999991, -0.55624999999999991, -0.46249999999999991, -0.36874999999999991, -0.27499999999999991, -0.18124999999999991, -0.087499999999999911, 0.0062500000000000888, 0.10000000000000009, 0.19375000000000009, 0.28750000000000009, 0.38125000000000009, 0.47500000000000009, 0.56875000000000009, 0.66250000000000009, 0.75625000000000009};
constexpr double kLfoPoints[] = {0.40000000000000002, 0.65000000000000002, 0.90000000000000002, 0.83928571428571441, 0.57142857142857151, 0.30357142857142871, 0.035714285714285837, -0.23214285714285701, -0.5, -0.421875, -0.34375, -0.265625, -0.1875, -0.109375, -0.03125, 0.15000000000000036, 0.40000000000000036, 0.65000000000000036, 0.90000000000000036, 0.83928571428571386, 0.57142857142857106, 0.30357142857142821, 0.035714285714285365, -0.23214285714285748};
constexpr double kLfoControlRate[] = {0.5, 0.5625, 0.63185061708409851, 0.70792263957735213, 0.7904600812325091, 0.8790850094683067, 0.97330467367648899, -0.92747920085172675, -0.82395912867545107, -0.71690880246603239, -0.60716850014268875, -0.49562923612252696, -0.38321595561329147, -0.27087008892663533, -0.15953179486059144, -0.050122228064467311, 0.056473835152950347, 0.15942468487169759, 0.25796736470684367, 0.35142206217233518, 0.43920493084123446, 0.52083910245948895, 0.59596368131024668, 0.66434055118313839};
constexpr double kHold[] = {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0.875, 0.875};
constexpr double kQuantize[] = {-0.75, -0.5, -0.25, 0, 0.25, 0.5, -0.5, -0.25, 0, 0.25, 0.5, 0.75, -0.5, -0.25, 0, 0.25, 0.5, -0.75, -0.5, -0.25, 0, 0.25, 0.5, -0.5};
constexpr double kLoop[] = {-1, -1.125, -0.8125, -0.28125, 0.359375, 1.0546875, -0.34765625, -0.673828125, -0.4619140625, 0.01904296875, 0.634521484375, 1.3172607421875, -0.09136962890625, -0.420684814453125, -0.2103424072265625, 0.26982879638671875, 0.88491439819335938, -0.55754280090332031, -0.90377140045166016, -0.70188570022583008, -0.22594285011291504, 0.38702857494354248, 1.0685142874717712, -0.34074285626411438};
constexpr double kNoiseWhite[] = {0.316650390625, -0.0150299072265625, -0.38726806640625, -0.2832183837890625, 0.297271728515625, -0.482330322265625, -0.2918701171875, -0.17333984375, -0.035736083984375, -0.3678436279296875, -0.0004119873046875, -0.13873291015625, 0.0014190673828125, -0.196685791015625, 0.425537109375, -0.428375244140625, -0.294281005859375, -0.4256134033203125, 0.490753173828125, -0.43377685546875, -0.397674560546875, 0.1865081787109375, -0.4225616455078125, -0.229583740234375};
constexpr double kNoisePink[] = {0.17374798291757812, 0.096486985776189277, -0.14133370160204453, -0.22772909959201668, 0.022900843412052934, -0.2661139390415444, -0.32439109920983167, -0.31407071297084005, -0.24490748794155212, -0.39305363328647575, -0.2823293814468894, -0.30620820314004554, -0.24356466231219201, -0.3194423781949291, -0.022836982296846824, -0.3194741029219858, -0.40618505615209205, -0.54610578330012527, -0.13873117372589341, -0.41712410285139956, -0.54433542679025115, -0.31614285888561083, -0.52783222629261528, -0.54083936727930837};
constexpr double kNoiseRadio[] = {0.14095536658268312, 0.071519287859126618, -0.13270765017285763, -0.1997067642103495, 0.021520771582775045, -0.20276606885442711, -0.2424302984547736, -0.21167502178812056, -0.1333566675130923, -0.23773738657408044, -0.13209315039495789, -0.13504880685041526, -0.074300802714571262, -0.12877987932692911, 0.11797160640871064, -0.12523201146295451, -0.20048334126093553, -0.30069888296207464, 0.051612113961967548, -0.16445640244268536, -0.26827232038432836, -0.06582901425701955, -0.22462679344406566, -0.22683335396837073};
constexpr double kNoiseDisc[] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, -0.196685791015625, 0, 0, 0, 0, 0, 0, 0, -0.4225616455078125, 0, 0, 0};
constexpr double kNoiseHum[] = {0, 0.15009274184387819, 0.27634477316748152, 0.36077323410744377, 0.39539527899519555, 0.38348055973206308, 0.33764630428951087, 0.2755027410028969, 0.21428571428571427, 0.16616461747001407, 0.13561579537906868, 0.11951497901455255, 0.10968099328090984, 0.096807653389933288, 0.074314264257039353, 0.040754618310995534, 2.6242431410300424e-17, -0.040754618310995479, -0.074314264257039325, -0.096807653389933246, -0.10968099328090987, -0.11951497901455253, -0.13561579537906868, -0.16616461747001396};
constexpr double kMapStepped[] = {3, 3, 3, 3, 3, 3, 3, 3, 1.5, 1.5, 1.5, 1.5, 1.5, 1.5, 1.5, 1.5, 0.10000000000000001, 0.10000000000000001, 0.10000000000000001, 0.10000000000000001, 0.10000000000000001, 0.10000000000000001, 0.10000000000000001, 0.10000000000000001, 7.25, 7.25, 7.25, 7.25, 7.25, 7.25, 7.25, 7.25, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9};
constexpr double kMapWindow[] = {0.29999999999999999, 0.29999999999999999, 0.29999999999999999, 0.29999999999999999, 0.29999999999999999, 0.29999999999999999, 0.29999999999999999, 0.29999999999999999, 0.29999999999999999, 0.29999999999999999, 0.29999999999999999, 0.29999999999999999, 0.29999999999999999, 0.29999999999999999, 0.29999999999999999, 0.29999999999999999, 0.29999999999999999, 0.29999999999999999, 0.29999999999999999, 0.29999999999999999, 0.29999999999999999, 0.13544303797468354, -0.0291139240506329, -0.1936708860759494, -0.35822784810126579, -0.5227848101265824, -0.68734177215189884, -0.85189873417721507, -1.0164556962025315, -1.181012658227848, -1.3455696202531646, -1.5101265822784808, -1.6746835443037975, -1.8392405063291137, -2.0037974683544304, -2.1683544303797473, -2.3329113924050633, -2.4974683544303802, -2.6620253164556962, -2.8265822784810131, -2.9911392405063295, -3.1556962025316455, -3.320253164556962, -3.4848101265822784, -3.6493670886075953, -3.8139240506329122, -3.9784810126582277, -4.1430379746835442, -4.3075949367088606, -4.472151898734178, -4.6367088607594944, -4.80126582278481, -4.9658227848101264, -5.1303797468354437, -5.2949367088607602, -5.4594936708860757, -5.6240506329113922, -5.7886075949367086, -5.953164556962026, -6.1177215189873424, -6.2822784810126588, -6.4468354430379744, -6.6113924050632908, -6.7759493670886082, -6.9405063291139237, -7.1050632911392411, -7.2696202531645566, -7.4341772151898731, -7.5987341772151904, -7.7632911392405068, -7.9278481012658242, -8.0924050632911388, -8.2569620253164544, -8.4215189873417717, -8.5860759493670873, -8.7506329113924046, -8.9151898734177202, -9.0797468354430375, -9.2443037974683548, -9.4088607594936704, -9.5734177215189877, -9.7379746835443033, -9.9025316455696188, -10.067088607594936, -10.231645569620252, -10.396202531645569, -10.560759493670886, -10.725316455696202, -10.889873417721519, -11.054430379746835, -11.21898734177215, -11.383544303797468, -11.548101265822783, -11.712658227848101, -11.877215189873416, -12.041772151898734, -12.206329113924051, -12.370886075949366, -12.535443037974684, -12.699999999999999, -12.699999999999999, -12.699999999999999, -12.699999999999999, -12.699999999999999, -12.699999999999999, -12.699999999999999, -12.699999999999999, -12.699999999999999, -12.699999999999999, -12.699999999999999, -12.699999999999999, -12.699999999999999, -12.699999999999999, -12.699999999999999, -12.699999999999999, -12.699999999999999, -12.699999999999999, -12.699999999999999, -12.699999999999999, -12.699999999999999, -12.699999999999999, -12.699999999999999, -12.699999999999999, -12.699999999999999, -12.699999999999999, -12.699999999999999, -12.699999999999999, -12.699999999999999};
constexpr double kMapStates[] = {0.10000000000000001, -1, -1, -1, -1, 2.5, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1};
constexpr double kMapPoints[] = {0.29999999999999999, 0.33499999999999996, 0.37, 0.40499999999999997, 0.43999999999999995, 0.47499999999999998, 0.51000000000000001, 0.54499999999999993, 0.57999999999999996, 0.61499999999999999, 0.64999999999999991, 0.68499999999999994, 0.71999999999999997, 0.75499999999999989, 0.78999999999999992, 0.82499999999999996, 0.85999999999999988, 0.89499999999999991, 0.92999999999999994, 0.96499999999999997, 0.99999999999999989, 1.0349999999999999, 1.0699999999999998, 1.105, 1.1399999999999999, 1.1749999999999998, 1.21, 1.2449999999999999, 1.2799999999999998, 1.3149999999999999, 1.3499999999999999, 1.3849999999999998, 1.4199999999999999, 1.4549999999999998, 1.4899999999999998, 1.5249999999999999, 1.5599999999999998, 1.5949999999999998, 1.6299999999999999, 1.6649999999999998, 1.7, 1.6810344827586206, 1.6620689655172414, 1.643103448275862, 1.6241379310344828, 1.6051724137931034, 1.5862068965517242, 1.5672413793103448, 1.5482758620689654, 1.5293103448275862, 1.5103448275862068, 1.4913793103448276, 1.4724137931034482, 1.4534482758620688, 1.4344827586206896, 1.4155172413793102, 1.396551724137931, 1.3775862068965516, 1.3586206896551725, 1.3396551724137931, 1.3206896551724137, 1.3017241379310345, 1.2827586206896551, 1.2637931034482759, 1.2448275862068965, 1.2258620689655173, 1.2068965517241379, 1.1879310344827585, 1.1689655172413793, 1.1499999999999999, 1.1310344827586207, 1.1120689655172413, 1.0931034482758619, 1.0741379310344827, 1.0551724137931033, 1.0362068965517242, 1.0172413793103448, 0.99827586206896546, 0.97931034482758617, 0.96034482758620687, 0.94137931034482758, 0.92241379310344829, 0.90344827586206888, 0.88448275862068959, 0.8655172413793103, 0.84655172413793101, 0.82758620689655171, 0.80862068965517242, 0.78965517241379302, 0.77068965517241372, 0.75172413793103443, 0.73275862068965514, 0.71379310344827585, 0.69482758620689655, 0.67586206896551726, 0.65689655172413786, 0.63793103448275856, 0.61896551724137927, 0.59999999999999998, 0.58103448275862069, 0.56206896551724139, 0.54310344827586199, 0.5241379310344827, 0.5051724137931034, 0.48620689655172411, 0.46724137931034482, 0.44827586206896547, 0.42931034482758618, 0.41034482758620688, 0.39137931034482754, 0.37241379310344824, 0.35344827586206895, 0.33448275862068966, 0.31551724137931031, 0.29655172413793102, 0.27758620689655172, 0.25862068965517238, 0.23965517241379308, 0.22068965517241376, 0.20172413793103447, 0.18275862068965515, 0.16379310344827583, 0.14482758620689654, 0.12586206896551722, 0.10689655172413791, 0.087931034482758602, 0.068965517241379282, 0.050000000000000003};
constexpr double kMapLogPoints[] = {19.999999999999996, 21.142486349148907, 22.350236451197397, 23.626978453484568, 24.976653471221645, 26.403427753136317, 27.911705542071203, 29.506142670235235, 31.191660931074431, 32.973463272126125, 34.857049855754475, 36.848235037344601, 38.953165313364444, 41.178338294697426, 43.53062276381381, 46.017279877694207, 48.645985581955735, 51.424854305369692, 54.362464007912507, 57.467882659669485, 60.750696232327648, 64.221038289663994, 67.889621268369496, 71.76776954576961, 75.867454396514916, 80.201330946149724, 84.782777235627279, 89.625935517359096, 94.745755910273289, 100.15804254863734, 105.87950236710208, 111.92779667255672, 118.32159566199232, 125.08063605465942, 132.22578201642506, 139.77908956439023, 147.76387465057925, 156.20478513486105, 165.12787686927697, 174.56069412863161, 184.53235463562751, 195.07363944300312, 206.21708795012438, 217.99709834733736, 230.45003379813252, 243.6143346868962, 257.53063727873473, 272.24189915766351, 287.79353183036363, 304.23354090484088, 321.61267427669162, 339.98457878041154, 359.40596578929978, 379.93678627514737, 401.64041586809071, 424.58385048787977, 448.8379131504542, 474.47747258819788, 501.5816743587327, 530.23418515563776, 560.52345107525866, 592.54297063682429, 626.39158339866037, 662.17377506139712, 699.99999999999977, 731.28530068140958, 763.96884427528619, 798.11312285297402, 833.78342146188015, 871.04794295275178, 909.97793838589519, 950.64784326568406, 993.1354198638287, 1037.5219059035403, 1083.8921698888855, 1132.3348733763146, 1182.9426404986348, 1235.8122350655785, 1291.0447455795679, 1348.7457785204408, 1409.0256602687184, 1471.9996480534753, 1537.7881503281644, 1606.5169569957725, 1678.317479923483, 1753.327004206729, 1831.6889506630764, 1913.5531500578072, 1999.0761295855466, 2088.4214121557079, 2181.7598290539754, 2279.2698465776539, 2381.1379072694435, 2487.5587864020563, 2598.7359643953009, 2714.8820158777307, 2836.2190161367143, 2962.9789657340993, 3095.4042340993624, 3233.7480229483735, 3378.2748504138858, 3529.2610568133764, 3686.9953330213716, 3851.7792724564124, 4023.9279477381565, 4203.7705131171806, 4391.6508338293534, 4587.9281435780895, 4792.9777314017228, 5007.1916592391581, 5230.9795115659354, 5464.7691785339848, 5709.0076741124767, 5964.1619907940267, 6230.7199925006371, 6509.1913473964287, 6800.1085023908981, 7104.0277011959133, 7421.5300478830395, 7753.2226179746658, 8099.7396191935877, 8461.7436040901575, 8839.9267368658111, 9235.0121168150708, 9647.7551609165494, 10078.945048216403, 10529.406228766198, 10999.999999999998};
constexpr double kMapTable[] = {0.050000000000000003, 0.15000000000000002, 0.25, 0.35000000000000003, 0.45000000000000001, 0.55000000000000004, 0.65000000000000013, 0.75000000000000011, 0.85000000000000009, 0.95000000000000007, 1.05, 1.1500000000000001, 1.2500000000000002, 1.3500000000000001, 1.4500000000000002, 1.55, 1.6500000000000001, 1.7500000000000002, 1.8500000000000001, 1.9500000000000002, 2.0499999999999998, 2.1499999999999999, 2.25, 2.3500000000000001, 2.4500000000000002, 2.5499999999999998, 2.6499999999999999, 2.75, 2.8500000000000001, 2.9500000000000002, 3.0499999999999998, 3.1499999999999999, 3.25, 3.3500000000000001, 3.4500000000000002, 3.5499999999999998, 3.6499999999999999, 3.75, 3.8500000000000001, 3.9500000000000002, 4.0499999999999998, 4.1500000000000004, 4.25, 4.3499999999999996, 4.4500000000000002, 4.5499999999999998, 4.6500000000000004, 4.75, 4.8500000000000005, 4.9500000000000002, 5.0499999999999998, 5.1500000000000004, 5.25, 5.3500000000000005, 5.4500000000000002, 5.5499999999999998, 5.6500000000000004, 5.75, 5.8500000000000005, 5.9500000000000002, 6.0499999999999998, 6.1500000000000004, 6.25, 6.3500000000000005, 6.4500000000000002, 6.5499999999999998, 6.6500000000000004, 6.75, 6.8500000000000005, 6.9500000000000002, 7.0499999999999998, 7.1500000000000004, 7.25, 7.3500000000000005, 7.4500000000000002, 7.5499999999999998, 7.6500000000000004, 7.75, 7.8500000000000005, 7.9500000000000002, 8.0500000000000007, 8.1500000000000004, 8.2500000000000018, 8.3500000000000014, 8.4500000000000011, 8.5500000000000007, 8.6500000000000004, 8.7500000000000018, 8.8500000000000014, 8.9500000000000011, 9.0500000000000007, 9.1500000000000004, 9.2500000000000018, 9.3500000000000014, 9.4500000000000011, 9.5500000000000007, 9.6500000000000021, 9.7500000000000018, 9.8500000000000014, 9.9500000000000011, 0.75000000000000011, 0.75000000000000011, 0.75000000000000011, 0.75000000000000011, 0.75000000000000011, 0.75000000000000011, 0.75000000000000011, 0.75000000000000011, 0.75000000000000011, 0.75000000000000011, 0.75000000000000011, 0.75000000000000011, 0.75000000000000011, 0.75000000000000011, 0.75000000000000011, 0.75000000000000011, 0.75000000000000011, 0.75000000000000011, 0.75000000000000011, 0.75000000000000011, 0.75000000000000011, 0.75000000000000011, 0.75000000000000011, 0.75000000000000011, 0.75000000000000011, 0.75000000000000011, 0.75000000000000011, 0.75000000000000011};
// clang-format on

}  // namespace

namespace {

namespace gc = sonare::midi::synth::gs_classic;
using gc::GsClassicNodeKind;
using gc::GsClassicValue;
using gc::GsClassicValueKind;

constexpr std::size_t kN = 24;

double in_l(std::size_t n) {
  return static_cast<double>(static_cast<int>((n * 37) % 17) - 8) / 8.0;
}
double in_r(std::size_t n) {
  return static_cast<double>(static_cast<int>((n * 11) % 13) - 6) / 6.0;
}

GsClassicValue konst(double v) { return {GsClassicValueKind::kConst, 0, gc::kGsClassicNone, 0, v}; }
GsClassicValue byte_value(uint8_t slot, uint16_t table) {
  return {GsClassicValueKind::kByte, slot, table, 0, 0.0};
}
GsClassicValue control(uint16_t signal, uint16_t curve = gc::kGsClassicNone) {
  return {GsClassicValueKind::kControl, 0, curve, signal, 0.0};
}

/// A one-type model set assembled by hand, in the pool layout the generator emits.
struct Model {
  std::vector<gc::GsClassicNode> nodes;
  std::vector<gc::GsClassicInput> inputs;
  std::vector<GsClassicValue> values;
  std::vector<gc::GsClassicComponent> components;
  std::vector<gc::GsClassicPoint> points;
  std::vector<gc::GsClassicPoints> point_lists;
  std::vector<gc::GsClassicPanLaw> laws;
  std::vector<gc::GsClassicCurve> curves;
  std::vector<gc::GsClassicMapSpec> specs;
  std::vector<uint8_t> keys;
  std::vector<double> map_values;
  std::vector<gc::GsClassicLut> luts;
  gc::GsClassicType type{};
  uint16_t next_signal = 2;

  uint16_t add(GsClassicNodeKind kind, std::initializer_list<uint16_t> in,
               std::initializer_list<GsClassicValue> vals, uint16_t aux = 0, uint8_t flags = 0) {
    gc::GsClassicNode node{};
    node.kind = kind;
    node.flags = flags;
    node.aux = aux;
    node.input_begin = static_cast<uint16_t>(inputs.size());
    node.n_inputs = static_cast<uint8_t>(in.size());
    node.value_begin = static_cast<uint16_t>(values.size());
    node.value_count = static_cast<uint8_t>(vals.size());
    for (uint16_t s : in) inputs.push_back({s});
    for (const auto& v : vals) values.push_back(v);
    nodes.push_back(node);
    const uint16_t first = next_signal;
    next_signal = static_cast<uint16_t>(next_signal + (kind == GsClassicNodeKind::kPan ? 2 : 1));
    return first;
  }

  /// Every node so far as its own loop-free component.
  void singles() {
    for (std::size_t i = components.size() == 0 ? 0 : components.back().node_end; i < nodes.size();
         ++i) {
      components.push_back({static_cast<uint16_t>(i), static_cast<uint16_t>(i + 1), 0});
    }
  }

  uint16_t map(const gc::GsClassicMapSpec& shape, std::initializer_list<uint8_t> k,
               std::initializer_list<double> v) {
    gc::GsClassicMapSpec spec = shape;
    spec.key_begin = static_cast<uint16_t>(keys.size());
    spec.value_begin = static_cast<uint16_t>(map_values.size());
    spec.n = static_cast<uint16_t>(v.size());
    keys.insert(keys.end(), k);
    map_values.insert(map_values.end(), v);
    specs.push_back(spec);
    gc::GsClassicLut lut{};
    REQUIRE(gc::gs_classic_expand_map(spec, keys.data(), map_values.data(), lut));
    luts.push_back(lut);
    return static_cast<uint16_t>(specs.size() - 1);
  }

  gc::GsClassicModelSet set(uint16_t out_l, uint16_t out_r, uint32_t max_delay = 64) {
    type.node_begin = 0;
    type.node_end = static_cast<uint16_t>(nodes.size());
    type.comp_begin = 0;
    type.comp_end = static_cast<uint16_t>(components.size());
    type.out_l = out_l;
    type.out_r = out_r;
    type.max_delay_samples = max_delay;
    gc::GsClassicModelSet s{};
    s.types = &type;
    s.n_types = 1;
    s.nodes = nodes.data();
    s.components = components.data();
    s.inputs = inputs.data();
    s.values = values.data();
    s.point_lists = {point_lists.data(), point_lists.size()};
    s.points = points.data();
    s.pan_laws = laws.data();
    s.curves = curves.data();
    s.map_specs = specs.data();
    s.n_map_specs = specs.size();
    s.map_keys = keys.data();
    s.map_values = map_values.data();
    s.luts = luts.data();
    return s;
  }
};

gc::GsClassicMapSpec shape(gc::GsClassicMapKind kind, uint16_t per_entry = 0, uint16_t oor = 0,
                           uint8_t log = 0) {
  gc::GsClassicMapSpec s{};
  s.kind = kind;
  s.log = log;
  s.per_entry = per_entry;
  s.out_of_range = oor;
  s.accept_lo = 0;
  s.accept_hi = 127;
  s.power_on = 0;
  return s;
}

struct Rendered {
  std::array<double, kN> l{};
  std::array<double, kN> r{};
};

Rendered render(gc::GsClassicGraph& graph) {
  std::array<double, kN> a{}, b{};
  for (std::size_t n = 0; n < kN; ++n) {
    a[n] = in_l(n);
    b[n] = in_r(n);
  }
  Rendered out;
  graph.process(a.data(), b.data(), out.l.data(), out.r.data(), kN);
  return out;
}

Rendered run(Model& m, uint16_t out_l, uint16_t out_r, std::size_t max_block = kN,
             uint32_t max_delay = 64) {
  const gc::GsClassicModelSet set = m.set(out_l, out_r, max_delay);
  gc::GsClassicGraph graph;
  REQUIRE(graph.prepare(set, m.type, max_block));
  graph.set_byte(0, 0);
  return render(graph);
}

template <std::size_t N>
void require_close(const std::array<double, kN>& got, const double (&want)[N], double tol) {
  static_assert(N == kN, "expected arrays hold kN samples");
  for (std::size_t n = 0; n < kN; ++n) {
    INFO("sample " << n << " got " << got[n] << " want " << want[n]);
    REQUIRE(std::fabs(got[n] - want[n]) <= tol);
  }
}

Rendered run_bytes(Model& m, uint16_t out_l, uint16_t out_r, uint8_t slot0,
                   std::size_t max_block = kN) {
  const gc::GsClassicModelSet set = m.set(out_l, out_r);
  gc::GsClassicGraph graph;
  REQUIRE(graph.prepare(set, m.type, max_block));
  graph.set_byte(0, slot0);
  return render(graph);
}

uint8_t lfo_flags(gc::GsClassicLfoShape s) { return static_cast<uint8_t>(s); }

}  // namespace

TEST_CASE("gain reads its byte through the expanded LUT", "[gs-classic-engine]") {
  Model m;
  const uint16_t t = m.map(shape(gc::GsClassicMapKind::kTable, 0, 3), {}, {0.25, 0.5, 0.75, 1.0});
  const uint16_t g = m.add(GsClassicNodeKind::kGain, {0}, {byte_value(0, t)});
  m.singles();
  require_close(run_bytes(m, g, 1, 2).l, kGain, 0.0);
}

TEST_CASE("mix sums sixteen weighted inputs in order", "[gs-classic-engine]") {
  Model m;
  std::vector<uint16_t> gains;
  for (int k = 0; k < 16; ++k) {
    gains.push_back(
        m.add(GsClassicNodeKind::kGain, {static_cast<uint16_t>(k % 2)}, {konst(1.0 / (k + 1))}));
  }
  const uint16_t w = m.map(shape(gc::GsClassicMapKind::kWindow), {10, 50}, {-1.0, 3.0});
  gc::GsClassicNode mix{};
  mix.kind = GsClassicNodeKind::kMix;
  mix.input_begin = static_cast<uint16_t>(m.inputs.size());
  mix.n_inputs = 16;
  mix.value_begin = static_cast<uint16_t>(m.values.size());
  mix.value_count = 16;
  for (int k = 0; k < 16; ++k) {
    m.inputs.push_back({gains[static_cast<std::size_t>(k)]});
    m.values.push_back(k == 3 ? byte_value(0, w) : konst(k == 5 ? 0.0 : (k - 7.5) / 8.0));
  }
  m.nodes.push_back(mix);
  const uint16_t out = m.next_signal++;
  m.singles();
  require_close(run_bytes(m, out, 1, 30).l, kMix16, 1e-15);
}

TEST_CASE("pan interpolates its law pair at a fractional position", "[gs-classic-engine]") {
  Model m;
  gc::GsClassicPanLaw law{};
  for (int i = 0; i < 128; ++i) {
    law.left[i] = static_cast<float>((127 - i) / 127.0);
    law.right[i] = static_cast<float>(i / 127.0);
  }
  m.laws.push_back(law);
  const uint16_t w = m.map(shape(gc::GsClassicMapKind::kWindow), {0, 64}, {0.0, 127.0});
  const uint16_t p = m.add(GsClassicNodeKind::kPan, {0}, {byte_value(0, w)}, 0);
  m.singles();
  const Rendered r = run_bytes(m, p, static_cast<uint16_t>(p + 1), 21);
  require_close(r.l, kPanLeft, 1e-7);
  require_close(r.r, kPanRight, 1e-7);
}

TEST_CASE("delay reads back through its interpolator and its modulating control",
          "[gs-classic-engine]") {
  Model m;
  const uint16_t l = m.add(GsClassicNodeKind::kLfo, {}, {konst(1000.0), konst(0.0)}, 0,
                           lfo_flags(gc::GsClassicLfoShape::kSine));
  const uint16_t d = m.add(GsClassicNodeKind::kDelay, {0}, {konst(0.1), konst(0.05), control(l)}, 0,
                           static_cast<uint8_t>(gc::GsClassicInterpolation::kLinear));
  const uint16_t d0 = m.add(GsClassicNodeKind::kDelay, {1}, {konst(0.1)}, 0,
                            static_cast<uint8_t>(gc::GsClassicInterpolation::kNone));
  m.singles();
  const Rendered r = run(m, d, d0);
  require_close(r.l, kDelayModulated, 1e-12);
  require_close(r.r, kDelayNone, 0.0);
}

TEST_CASE("a block longer than the longest delay reads back what the block wrote",
          "[gs-classic-engine]") {
  Model m;
  const uint16_t d0 = m.add(GsClassicNodeKind::kDelay, {1}, {konst(0.1)}, 0,
                            static_cast<uint8_t>(gc::GsClassicInterpolation::kNone));
  m.singles();
  // 0.1 ms is 3.2 samples; a longest delay of 4 alone would size the ring to 8, under kN.
  require_close(run(m, 1, d0, kN, 4).r, kDelayNone, 0.0);
}

TEST_CASE("a delay a float curve puts just under a whole sample reads that sample",
          "[gs-classic-engine]") {
  Model m;
  // Three samples at 32 kHz, stored as the float just under it.
  gc::GsClassicCurve curve{};
  for (float& v : curve.v) v = std::nextafter(static_cast<float>(3.0 * 1000.0 / 32000.0), 0.0f);
  curve.lo = -1.0f;
  curve.hi = 1.0f;
  m.curves.push_back(curve);
  REQUIRE(static_cast<double>(curve.v[0]) * 32.0 < 3.0);
  const uint16_t a = m.add(GsClassicNodeKind::kLfo, {}, {konst(0.0), konst(0.0)}, 0,
                           lfo_flags(gc::GsClassicLfoShape::kSine));
  const uint16_t d = m.add(GsClassicNodeKind::kDelay, {0}, {control(a, 0)}, 0,
                           static_cast<uint8_t>(gc::GsClassicInterpolation::kNone));
  m.singles();
  const Rendered r = run(m, d, 1);
  for (std::size_t n = 0; n < kN; ++n) {
    INFO("sample " << n);
    REQUIRE(r.l[n] == (n < 3 ? 0.0 : in_l(n - 3)));
  }
}

TEST_CASE("lfo draws each shape from phase zero plus its offset", "[gs-classic-engine]") {
  const std::pair<gc::GsClassicLfoShape, const double*> shapes[] = {
      {gc::GsClassicLfoShape::kSine, kLfo_sine},
      {gc::GsClassicLfoShape::kTriangle, kLfo_triangle},
      {gc::GsClassicLfoShape::kSquare, kLfo_square},
      {gc::GsClassicLfoShape::kSaw, kLfo_saw},
  };
  for (const auto& [s, want] : shapes) {
    Model m;
    const uint16_t l =
        m.add(GsClassicNodeKind::kLfo, {}, {konst(1500.0), konst(0.3)}, 0, lfo_flags(s));
    m.singles();
    const Rendered r = run(m, l, 1);
    for (std::size_t n = 0; n < kN; ++n) {
      INFO("shape " << static_cast<int>(s) << " sample " << n);
      REQUIRE(std::fabs(r.l[n] - want[n]) <= 1e-12);
    }
  }
  Model m;
  m.points = {{0.0, 0.0}, {0.25, 1.0}, {0.6, -0.5}};
  m.point_lists.push_back({0, 3});
  const uint16_t l = m.add(GsClassicNodeKind::kLfo, {}, {konst(2000.0), konst(0.1)}, 0,
                           lfo_flags(gc::GsClassicLfoShape::kPoints));
  m.singles();
  require_close(run(m, l, 1).l, kLfoPoints, 1e-12);
}

TEST_CASE("lfo integrates a control-driven rate sample by sample", "[gs-classic-engine]") {
  Model m;
  gc::GsClassicCurve curve{};
  for (std::size_t j = 0; j < gc::kGsClassicCurveSize; ++j) {
    curve.v[j] = static_cast<float>(200.0 + 1600.0 * static_cast<double>(j) / 255.0);
  }
  curve.lo = -1.0f;
  curve.hi = 1.0f;
  m.curves.push_back(curve);
  const uint16_t a = m.add(GsClassicNodeKind::kLfo, {}, {konst(700.0), konst(0.0)}, 0,
                           lfo_flags(gc::GsClassicLfoShape::kSine));
  const uint16_t b = m.add(GsClassicNodeKind::kLfo, {}, {control(a, 0), konst(0.25)}, 0,
                           lfo_flags(gc::GsClassicLfoShape::kSaw));
  m.singles();
  require_close(run(m, b, 1).l, kLfoControlRate, 1e-6);
}

TEST_CASE("hold takes its input on each tick of its rate", "[gs-classic-engine]") {
  Model m;
  const uint16_t h = m.add(GsClassicNodeKind::kHold, {0}, {konst(3000.0)});
  m.singles();
  require_close(run(m, h, 1).l, kHold, 0.0);
  // Drawn a sample at a time, the ticks land on the same samples.
  require_close(run(m, h, 1, 1).l, kHold, 0.0);
}

TEST_CASE("quantize rounds to its word and saturates", "[gs-classic-engine]") {
  Model m;
  const uint16_t g = m.add(GsClassicNodeKind::kGain, {0}, {konst(0.7)});
  const uint16_t q = m.add(GsClassicNodeKind::kQuantize, {g}, {}, 3);
  m.singles();
  require_close(run(m, q, 1).l, kQuantize, 0.0);
}

TEST_CASE("x-noise draws each source from a seed fixed per render", "[gs-classic-engine]") {
  const std::pair<gc::GsClassicNoise, std::pair<double, const double*>> sources[] = {
      {gc::GsClassicNoise::kWhite, {0.0, kNoiseWhite}},
      {gc::GsClassicNoise::kPink, {0.0, kNoisePink}},
      {gc::GsClassicNoise::kRadio, {3000.0, kNoiseRadio}},
      {gc::GsClassicNoise::kDisc, {8000.0, kNoiseDisc}},
      {gc::GsClassicNoise::kHum, {1000.0, kNoiseHum}},
  };
  for (const auto& [kind, spec] : sources) {
    Model m;
    const uint16_t x = m.add(GsClassicNodeKind::kXNoise, {}, {konst(0.5), konst(spec.first)}, 7,
                             static_cast<uint8_t>(kind));
    m.singles();
    const gc::GsClassicModelSet set = m.set(x, 1);
    gc::GsClassicGraph graph;
    REQUIRE(graph.prepare(set, m.type, kN));
    for (int pass = 0; pass < 2; ++pass) {
      const Rendered r = render(graph);
      for (std::size_t n = 0; n < kN; ++n) {
        INFO("noise " << static_cast<int>(kind) << " pass " << pass << " sample " << n);
        REQUIRE(std::fabs(r.l[n] - spec.second[n]) <= 1e-12);
      }
      graph.reset();
    }
  }
}

TEST_CASE("a loop closes through its delay one sample late", "[gs-classic-engine]") {
  for (double time_ms : {1000.0 / 32000.0, 0.01}) {
    Model m;
    // In the renderer's same-sample order: delay (signal 2), gain (3), mix (4).
    m.add(GsClassicNodeKind::kDelay, {4}, {konst(time_ms)}, 0,
          static_cast<uint8_t>(gc::GsClassicInterpolation::kNone));
    m.add(GsClassicNodeKind::kGain, {2}, {konst(0.5)});
    const uint16_t mx = m.add(GsClassicNodeKind::kMix, {0, 3}, {konst(1.0), konst(1.0)});
    m.components.push_back({0, 3, 1});
    INFO("time_ms " << time_ms << " (under one sample is drawn as one)");
    require_close(run(m, mx, 1).l, kLoop, 0.0);
  }
}

TEST_CASE("a byte change re-indexes the LUT without a new prepare", "[gs-classic-engine]") {
  Model m;
  const uint16_t t = m.map(shape(gc::GsClassicMapKind::kTable, 0, 3), {}, {0.25, 0.5, 0.75, 1.0});
  const uint16_t g = m.add(GsClassicNodeKind::kGain, {0}, {byte_value(0, t)});
  m.singles();
  const gc::GsClassicModelSet set = m.set(g, 1);
  gc::GsClassicGraph graph;
  REQUIRE(graph.prepare(set, m.type, 8));
  graph.set_byte(0, 2);
  std::array<double, kN> a{}, b{}, l{}, r{};
  for (std::size_t n = 0; n < kN; ++n) a[n] = in_l(n);
  graph.process(a.data(), b.data(), l.data(), r.data(), 12);
  graph.set_byte(0, 1);
  graph.process(a.data() + 12, b.data(), l.data() + 12, r.data(), 6);
  graph.set_byte(0, 90);  // past the table: entry out_of_range
  graph.process(a.data() + 18, b.data(), l.data() + 18, r.data(), 6);
  for (std::size_t n = 0; n < kN; ++n) {
    const double gain = n < 12 ? 0.75 : n < 18 ? 0.5 : 1.0;
    INFO("sample " << n);
    REQUIRE(l[n] == a[n] * gain);
  }
  REQUIRE(graph.byte(0) == 90);
}

namespace {

void require_map(const gc::GsClassicMapSpec& s, std::initializer_list<uint8_t> k,
                 std::initializer_list<double> v, const double (&want)[128], int ulps) {
  Model m;
  const uint16_t i = m.map(s, k, v);
  const gc::GsClassicLut& lut = m.luts[i];
  for (int b = 0; b < 128; ++b) {
    const float expect = static_cast<float>(want[b]);
    float lo = expect, hi = expect;
    for (int u = 0; u < ulps; ++u) {
      lo = std::nextafter(lo, -INFINITY);
      hi = std::nextafter(hi, INFINITY);
    }
    INFO("byte " << b << " got " << lut.v[b] << " want " << expect);
    REQUIRE(lut.v[b] >= lo);
    REQUIRE(lut.v[b] <= hi);
  }
}

}  // namespace

TEST_CASE("the map expander agrees with _from_map on every rule", "[gs-classic-engine]") {
  using K = gc::GsClassicMapKind;
  require_map(shape(K::kSteppedTable, 8), {}, {3.0, 1.5, 0.1, 7.25, 9.0}, kMapStepped, 0);
  require_map(shape(K::kWindow), {20, 99}, {0.3, -12.7}, kMapWindow, 0);
  require_map(shape(K::kStates), {0, 5, gc::kGsClassicStateWildcard}, {0.1, 2.5, -1.0}, kMapStates,
              0);
  require_map(shape(K::kPoints), {0, 40, 127}, {0.3, 1.7, 0.05}, kMapPoints, 0);
  require_map(shape(K::kPoints, 0, 0, 1), {0, 64, 127}, {20.0, 700.0, 11000.0}, kMapLogPoints, 1);
  std::vector<double> entries;
  for (int k = 0; k < 100; ++k) entries.push_back(0.1 * k + 0.05);
  Model m;
  gc::GsClassicMapSpec table = shape(K::kTable, 0, 7);
  table.value_begin = 0;
  table.n = 100;
  gc::GsClassicLut lut{};
  REQUIRE(gc::gs_classic_expand_map(table, nullptr, entries.data(), lut));
  for (int b = 0; b < 128; ++b) {
    INFO("byte " << b);
    REQUIRE(lut.v[b] == static_cast<float>(kMapTable[b]));
  }
  table.out_of_range = 100;
  REQUIRE_FALSE(gc::gs_classic_expand_map(table, nullptr, entries.data(), lut));
}

TEST_CASE("an unnamed state snaps to the nearest named one when the protocol accepts it",
          "[gs-classic-engine]") {
  gc::GsClassicMapSpec s = shape(gc::GsClassicMapKind::kStates);
  s.accept_lo = 0;
  s.accept_hi = 7;
  s.power_on = 5;
  Model m;
  const gc::GsClassicLut& lut = m.luts[m.map(s, {0, 5, 9}, {0.1, 2.5, 4.0})];
  const float want[] = {0.1f, 0.1f, 0.1f, 2.5f, 2.5f, 2.5f, 2.5f, 2.5f, 2.5f, 4.0f};
  for (int b = 0; b < 10; ++b) {
    INFO("byte " << b << " (7 ties between 5 and 9 and takes the lower; 8 is rejected)");
    REQUIRE(lut.v[b] == want[b]);
  }
  REQUIRE(lut.v[100] == 2.5f);
}
