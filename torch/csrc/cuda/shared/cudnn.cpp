#include <torch/csrc/utils/pybind.h>
#include <cudnn.h>

namespace torch { namespace cuda { namespace shared {

void initCudnnBindings(PyObject* module) {
  auto m = py::handle(module).cast<py::module>();

  auto cudnn = m.def_submodule("_cudnn", "libcudnn.so bindings");

  py::enum_<cudnnRNNMode_t>(cudnn, "RNNMode")
    .value("rnn_relu", CUDNN_RNN_RELU)
    .value("rnn_tanh", CUDNN_RNN_TANH)
    .value("lstm", CUDNN_LSTM)
    .value("gru", CUDNN_GRU);

  cudnn.def("getVersion", cudnnGetVersion);
}

} // namespace shared
} // namespace cuda
} // namespace torch

