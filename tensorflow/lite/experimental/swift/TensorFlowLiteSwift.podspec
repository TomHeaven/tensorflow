Pod::Spec.new do |s|
  s.name             = 'TensorFlowLiteSwift'
  s.version          = '2.3.0'
  s.authors          = 'Google Inc.'
  s.license          = { :type => 'Apache' }
  s.homepage         = 'https://github.com/tensorflow/tensorflow'
  s.source           = { :git => 'https://github.com/tensorflow/tensorflow.git', :tag => "v#{s.version}" }
  s.summary          = 'TensorFlow Lite for Swift'
  s.description      = <<-DESC

  TensorFlow Lite is TensorFlow's lightweight solution for Swift developers. It
  enables low-latency inference of on-device machine learning models with a
  small binary size and fast performance supporting hardware acceleration.
                       DESC

  s.ios.deployment_target = '9.0'

  s.module_name = 'TensorFlowLite'
  s.static_framework = true

  tfl_dir = 'tensorflow/lite/'
  swift_dir = tfl_dir + 'experimental/swift/'

  tfl_dir = 'tensorflow/lite/'
  swift_dir = tfl_dir + 'experimental/swift/'

  s.default_subspec = 'Core'

  s.subspec 'Core' do |core|
    core.dependency 'TensorFlowLiteC', "#{s.version}"
    core.source_files = swift_dir + 'Sources/*.swift'
    core.exclude_files = swift_dir + 'Sources/{CoreML,Metal}Delegate.swift'

    core.test_spec 'Tests' do |ts|
      ts.source_files = swift_dir + 'Tests/*.swift'
      ts.exclude_files = swift_dir + 'Tests/MetalDelegateTests.swift'
      ts.resources = [
        tfl_dir + 'testdata/add.bin',
        tfl_dir + 'testdata/add_quantized.bin',
      ]
    end
  end

  s.subspec 'CoreML' do |coreml|
    coreml.source_files = swift_dir + 'Sources/CoreMLDelegate.swift'
    coreml.dependency 'TensorFlowLiteC/CoreML', "#{s.version}"
    coreml.dependency 'TensorFlowLiteSwift/Core', "#{s.version}"
  end

  s.subspec 'Metal' do |metal|
    metal.source_files = swift_dir + 'Sources/MetalDelegate.swift'
    metal.dependency 'TensorFlowLiteC/Metal', "#{s.version}"
    metal.dependency 'TensorFlowLiteSwift/Core', "#{s.version}"

    metal.test_spec 'Tests' do |ts|
      ts.source_files = swift_dir + 'Tests/{Interpreter,MetalDelegate}Tests.swift'
      ts.resources = [
        tfl_dir + 'testdata/add.bin',
        tfl_dir + 'testdata/add_quantized.bin',
      ]
    end
  end
end
