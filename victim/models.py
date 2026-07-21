MODELS = [

    ("resnet18",            "ResNet",     "resnet18",            224),
    ("resnet34",            "ResNet",     "resnet34",            224),
    ("resnet50",            "ResNet",     "resnet50",            224),
    ("resnet101",           "ResNet",     "resnet101",           224),
    ("resnet152",           "ResNet",     "resnet152",           224),

    ("vgg11",               "VGG",        "vgg11",               224),
    ("vgg13",               "VGG",        "vgg13",               224),
    ("vgg16",               "VGG",        "vgg16",               224),
    ("vgg19",               "VGG",        "vgg19",               224),
    ("vgg11_bn",            "VGG",        "vgg11_bn",            224),
    ("vgg13_bn",            "VGG",        "vgg13_bn",            224),
    ("vgg16_bn",            "VGG",        "vgg16_bn",            224),

    ("densenet121",         "DenseNet",   "densenet121",         224),
    ("densenet161",         "DenseNet",   "densenet161",         224),
    ("densenet169",         "DenseNet",   "densenet169",         224),
    ("densenet201",         "DenseNet",   "densenet201",         224),

    ("squeezenet1_0",       "SqueezeNet", "squeezenet1_0",       224),
    ("squeezenet1_1",       "SqueezeNet", "squeezenet1_1",       224),

    ("shufflenet_v2_x0_5",  "ShuffleNet", "shufflenet_v2_x0_5",  224),
    ("shufflenet_v2_x1_0",  "ShuffleNet", "shufflenet_v2_x1_0",  224),

    ("resnext50_32x4d",     "ResNeXt",    "resnext50_32x4d",     224),
    ("resnext101_32x8d",    "ResNeXt",    "resnext101_32x8d",    224),

    ("wide_resnet50_2",     "WideResNet", "wide_resnet50_2",     224),
    ("wide_resnet101_2",    "WideResNet", "wide_resnet101_2",    224),

    ("mnasnet0_5",          "MNASNet",    "mnasnet0_5",          224),
    ("mnasnet1_0",          "MNASNet",    "mnasnet1_0",          224),

    ("mobilenet_v2",        "MobileNet",  "mobilenet_v2",        224),
    ("inception_v3",        "Inception",  "inception_v3",        299),
    ("googlenet",           "GoogLeNet",  "googlenet",           224),
    ("alexnet",             "AlexNet",    "alexnet",             224),
]

INSTANCE_NAMES = [m[0] for m in MODELS]
ARCH_CLASSES   = sorted(set(m[1] for m in MODELS))
INSTANCE_ID    = {m[0]: i for i, m in enumerate(MODELS)}
ARCH_ID        = {a: i for i, a in enumerate(ARCH_CLASSES)}
INSTANCE_ARCH  = {m[0]: m[1] for m in MODELS}

_FAST_INIT = False

def _fast_init():
    global _FAST_INIT
    if _FAST_INIT:
        return
    import torch.nn.init as I
    def noop(t, *a, **k):
        return t
    for fn in ("kaiming_normal_", "kaiming_uniform_", "xavier_normal_",
               "xavier_uniform_", "normal_", "uniform_", "constant_",
               "ones_", "zeros_", "trunc_normal_"):
        if hasattr(I, fn):
            setattr(I, fn, noop)
    _FAST_INIT = True

def build(name):
    import torch, torchvision.models as tvm
    _fast_init()
    ctor = dict((m[0], m[2]) for m in MODELS)[name]
    insz = dict((m[0], m[3]) for m in MODELS)[name]
    kw = {"pretrained": False}
    if ctor in ("googlenet", "inception_v3"):
        kw["aux_logits"] = False
    if ctor == "googlenet":
        kw["init_weights"] = False
    model = getattr(tvm, ctor)(**kw)
    model.eval()
    return model, insz

if __name__ == "__main__":
    print(f"{len(MODELS)} instances, {len(ARCH_CLASSES)} architecture classes:")
    for a in ARCH_CLASSES:
        insts = [m[0] for m in MODELS if m[1] == a]
        print(f"  {a:12s} ({len(insts)}): {', '.join(insts)}")
