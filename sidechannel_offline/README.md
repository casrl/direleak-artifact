# Artifact 7 — ML-model fingerprinting side channel (Figures 13 & 14)

Classifies a shipped subset of real HitME traces with the pre-trained classifiers
and regenerates the instance confusion matrix (Figure 13) and the memorygrams
(Figure 14).

Pick artifact 7 in the orchestrator menu, or run it from the repo root:

```bash
python3 sidechannel_offline/classify_offline.py
```

It needs `numpy` and `matplotlib`, and takes a few seconds. No second machine,
no root, no `msr` module, no counter server, no scikit-learn, no collection.

## Why this one ships offline

The live attack needs a dual-socket Intel Xeon with the memory directory cache,
root for the MSR counter server, and ~30 min of collection per run — and the
classifiers behind Figure 13 are trained on far more traces than one run
produces. Rather than ask reviewers to re-collect, this directory ships the
trained classifiers together with a subset of the real attack traces and replays
the classification. The traces are measurements from the real machine; none of
them are synthetic.

## Contents

| Path | What it is |
|---|---|
| `models/fingerprint_mlp.npz` | The architecture and instance classifiers, 70 MB |
| `data/subset_traces.npz` | 300 real attack traces, 10 per model instance, 16 MB |
| `classify_offline.py` | Loads both, classifies, writes Figures 13 and 14 |

## The traces

Each trace is a memorygram: 39 consecutive samples (200 ms) of per-set HitME
miss counts over 5000 directory sets — a 39×5000 integer image, flattened
to 195,000 values. This is the representation used for Figures 13 and 14.

The shipped traces come from the paper's online/attack set, which is disjoint
from the offline set the classifiers were trained on, so this is a held-out
evaluation rather than a replay of training data. All 30 model instances are
covered, 10 traces each, across 15 architecture families.

## The classifiers

Two 3-layer MLPs, one over the 15 architecture families and one over the 30
instances, trained on the disjoint offline traces. Preprocessing is
`log1p(max(count, 0))` followed by standardization.

Weights are stored as bare `float16` arrays and evaluated by the numpy forward
pass in `classify_offline.py` rather than unpickled as scikit-learn estimators.
That keeps the file committable and keeps the artifact from having to match the
scikit-learn version it was trained with.

## Expected output

```
  ┌───────────────┬──────────┬──────────┬────────────────────────┐
  │ classifier    │ accuracy │  top-3   │ classes (chance)       │
  ├───────────────┼──────────┼──────────┼────────────────────────┤
  │ architecture  │   92.7%  │   97.3%  │ 15 classes (6.7%)      │
  │ instance      │   76.3%  │   83.3%  │ 30 classes (3.3%)      │
  └───────────────┴──────────┴──────────┴────────────────────────┘
```

Figures land in `output/figures/` as PNG and SVG:

* `sc_confusion.png` — Figure 13, the 30×30 instance confusion matrix
* `sc_confusion_arch.png` — the 15×15 architecture-family confusion matrix
* `sc_memorygram.png` — Figure 14, two memorygrams each for `VGG_16`, `VGG_19`,
  `ViT_B_16` and `ConvNeXt_Base`

What to look for: a bright block diagonal, far above the 3.3% chance line.
Classification is not perfect, and the residual confusions are the informative
part. Most pair up models from one family — `RegNetX_6GF`↔`RegNetY_8GF`,
`ResNet_152`↔`ResNet_50`, `DenseNet_121`→`DenseNet_169`, `Swin_T`→`Swin_B`,
`VGG_11`→`VGG_19`, `DeiT_S`→`ViT_B_16`. The rest sit among the lightweight CNNs,
where `MobileNetV3_Large`, `EfficientNet_B0`/`B4`, `ShuffleNetV2_1_0x` and
`SqueezeNet_v1_1` trade places. Nine instances come out exactly right:
`ConvNeXt_Base`, `Inception_v3`, `MaxViT_Base`, `MnasNet_A1`, `MobileNetV2`,
`ResNet_18`, `ShuffleNetV2_1_0x`, `Swin_B` and `VGG_19`. Grouped into the 15
architecture families the picture is cleaner still, at 92.7%.

In the memorygrams each column is one model and the two rows are two independent
traces of it. The horizontal banding repeats between a model's own two samples
and differs between models — that per-model structure is what the classifier
keys on.

## Relation to the numbers in the paper

The paper reports 95.1% architecture and 83.9% instance accuracy, training on
5,000 traces per model. The classifiers here are trained on 300 traces per
model — what fits in a repository — so they land a few points lower. The
qualitative result is unchanged: a strong block diagonal, with most confusions
falling between closely related models.

The live collection path (`analysis/collect_sidechannel.py`,
`analysis/train_sidechannel.py`) remains in the repository for reference.
