"""TensorFlow Lite Build Configurations for iOS"""

# Placeholder for Google-internal load statements.
load("@build_bazel_rules_apple//apple:ios.bzl", "ios_static_framework")

TFL_MINIMUM_OS_VERSION = "9.0"

# Default tags for filtering iOS targets. Targets are restricted to Apple platforms.
TFL_DEFAULT_TAGS = [
    "apple",
]

# Following sanitizer tests are not supported by iOS test targets.
TFL_DISABLED_SANITIZER_TAGS = [
    "noasan",
    "nomsan",
    "notsan",
]

# iOS static framework with symbol allowlist. Exported C++ symbbols might cause
# symbol collision with other libraries. List of symbols to allowlist can be
# generated by running `nm -m -g FRAMEWORK_LIBRARY | grep _TfLite` for framework
# built with `ios_static_framework` rule.
def tflite_ios_static_framework(
        name,
        bundle_name,
        allowlist_symbols_file,
        exclude_resources = True,
        **kwargs):
    """TFLite variant of ios_static_framework with symbol hiding.

    Args:
      name: The name of the target.
      bundle_name: The name to give to the framework bundle, without the
          ".framework" extension. If omitted, the target's name will be used.
      allowlist_symbols_file: a file including a list of allowed symbols,
          one symbol per line.
      exclude_resources: Indicates whether resources should be excluded from the
          bundle. This can be used to avoid unnecessarily bundling resources if
          the static framework is being distributed in a different fashion, such
          as a Cocoapod.
      **kwargs: Pass-through arguments.
    """

    preprocessed_name = "Preprocessed_" + name
    ios_static_framework(
        name = preprocessed_name,
        bundle_name = bundle_name,
        exclude_resources = exclude_resources,
        **kwargs
    )

    framework_target = ":{}.zip".format(preprocessed_name)

    srcs = [
        framework_target,
        allowlist_symbols_file,
    ]
    cmd = ("INPUT_FRAMEWORK=\"$(location " + framework_target + ")\" " +
           "BUNDLE_NAME=\"" + bundle_name + "\" " +
           "ALLOWLIST_FILE_PATH=\"$(location " + allowlist_symbols_file + ")\" " +
           "OUTPUT=\"$(OUTS)\" " +
           "\"$(location //tensorflow/lite/experimental/ios:hide_symbols_with_allowlist)\"")

    native.genrule(
        name = name,
        srcs = srcs,
        outs = [name + ".zip"],
        cmd = cmd,
        tools = [
            "//tensorflow/lite/experimental/ios:hide_symbols_with_allowlist",
        ],
    )
