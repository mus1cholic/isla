#include "shaderc.h"

namespace bgfx {

bool compileGLSLShader(const Options& _options, uint32_t _version, const std::string& _code,
                       bx::WriterI* _writer, bx::WriterI* _messages) {
    (void)_options;
    (void)_version;
    (void)_code;
    (void)_writer;
    (void)_messages;
    return false;
}

bool compileMetalShader(const Options& _options, uint32_t _version, const std::string& _code,
                        bx::WriterI* _writer, bx::WriterI* _messages) {
    (void)_options;
    (void)_version;
    (void)_code;
    (void)_writer;
    (void)_messages;
    return false;
}

bool compilePSSLShader(const Options& _options, uint32_t _version, const std::string& _code,
                       bx::WriterI* _writer, bx::WriterI* _messages) {
    (void)_options;
    (void)_version;
    (void)_code;
    (void)_writer;
    (void)_messages;
    return false;
}

bool compileSPIRVShader(const Options& _options, uint32_t _version, const std::string& _code,
                        bx::WriterI* _writer, bx::WriterI* _messages) {
    (void)_options;
    (void)_version;
    (void)_code;
    (void)_writer;
    (void)_messages;
    return false;
}

const char* getPsslPreamble() {
    return "";
}

} // namespace bgfx
