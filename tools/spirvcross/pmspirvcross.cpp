// pmspirvcross: Vulkan shader stages (SPIR-V, or SMOL-V as Unity stores them) -> GLSL ES with
// Khronos' SPIRV-Cross. The converter (convert/pmunity/shaders/vulkan.py) runs it to give shaders
// that only carry Vulkan programs GLES3 ones. build.sh fetches SPIRV-Cross and SMOL-V and builds this.
//
//   pmspirvcross [-e 300|310] <stage.spv|stage.smolv>...    translate each stage, print the GLSL
//   pmspirvcross -d <in.smolv> <out.spv>                    decode a SMOL-V module
//   pmspirvcross -s                                         serve requests on stdin (below)
//
// What the translation does besides SPIRV-Cross's GLSL ES output (the converter names the
// variables, uniform blocks and their members in the module before; OpName / OpMemberName):
//   * every image is combined with the sampler(s) it is sampled with, as GLSL has it; the combined
//     sampler takes the image's name (a second sampler of the same image: <image>_PMSAMPLER<n>),
//     an image only read with texelFetch gets a dummy sampler;
//   * descriptor sets and bindings are dropped (GLSL ES 3.00 has none; Unity's GLES backend binds
//     textures and uniform blocks by name), and so are the locations of vertex inputs (Unity binds
//     its vertex channels by input name, as for HLSLcc's GL programs);
//   * default precision highp for floats and ints in fragment shaders, as desktop GL computes.
//
// Serve protocol (binary-safe, one request at a time):
//   request  "<op> <arg> <length>\n", then <length> bytes
//            op d: SMOL-V -> SPIR-V (arg 1: try SMOL-V's older encoding first, for Unity 2017 - 2019);
//            op c: SPIR-V or SMOL-V -> GLSL ES <arg> (300, 310)
//   answer   "<status> <length> <log length>\n", the output, then the log. status 0: done; 1: failed
//   a request with op q ends the server.
#include "smolv.h"
#include "spirv_glsl.hpp"
#include "spirv_parser.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace {

const uint32_t SPIRV_MAGIC = 0x07230203;

// SMOL-V's magic, 0x534D4F4C ("SMOL") as a little-endian word
bool is_smolv(const std::string &b)
{
    return b.size() >= 4 && !b.compare(0, 4, "LOMS");
}

// SPIR-V's header and instruction lengths add up to the module's size
bool well_formed(const std::string &spv)
{
    if (spv.size() < 20 || spv.size() % 4)
        return false;
    std::vector<uint32_t> w(spv.size() / 4);
    memcpy(w.data(), spv.data(), spv.size());
    if (w[0] != SPIRV_MAGIC)
        return false;
    size_t i = 5;
    while (i < w.size()) {
        uint32_t n = w[i] >> 16;
        if (!n || i + n > w.size())
            return false;
        i += n;
    }
    return i == w.size();
}

bool parses(const std::string &spv)
{
    std::vector<uint32_t> w(spv.size() / 4);
    memcpy(w.data(), spv.data(), w.size() * 4);
    try {
        spirv_cross::Parser parser(std::move(w));
        parser.parse();
    } catch (const std::exception &) {
        return false;
    }
    return true;
}

// SMOL-V -> SPIR-V; false (with a message in log) if the data is neither
bool decode(const std::string &in, std::string &out, std::string &log, bool older = false)
{
    if (!is_smolv(in)) {
        uint32_t m = 0;
        if (in.size() >= 20 && in.size() % 4 == 0 && (memcpy(&m, in.data(), 4), m == SPIRV_MAGIC)) {
            out = in;
            return true;
        }
        log += "error: neither SPIR-V nor SMOL-V\n";
        return false;
    }
    size_t n = smolv::GetDecodedBufferSize(in.data(), in.size());
    if (!n) {
        log += "error: bad SMOL-V header\n";
        return false;
    }
    // SMOL-V's "version 0" streams come in two encodings that the header does not tell apart:
    // Unity 2017 - 2019 (and early 2020) wrote the older one (smolv's 2016-08-31 code). older:
    // try that one first. The first that decodes into SPIR-V that SPIRV-Cross parses is taken.
    uint32_t modes[2] = {(uint32_t)smolv::kDecodeFlagNone, (uint32_t)smolv::kDecodeFlagUse20160831AsZeroVersion};
    if (older)
        std::swap(modes[0], modes[1]);
    for (uint32_t flags : modes) {
        out.assign(n, '\0');
        if (smolv::Decode(in.data(), in.size(), &out[0], n, flags) && well_formed(out) && parses(out))
            return true;
    }
    log += "error: SMOL-V decoding failed\n";
    return false;
}

bool translate(const std::string &module, int es, std::string &out, std::string &log)
{
    std::string spv;
    if (!decode(module, spv, log))
        return false;
    std::vector<uint32_t> words(spv.size() / 4);
    memcpy(words.data(), spv.data(), words.size() * 4);
    try {
        spirv_cross::CompilerGLSL comp(std::move(words));
        auto opts = comp.get_common_options();
        opts.es = true;
        opts.version = es;
        opts.vulkan_semantics = false;
        opts.enable_420pack_extension = false;
        opts.fragment.default_float_precision = spirv_cross::CompilerGLSL::Options::Highp;
        opts.fragment.default_int_precision = spirv_cross::CompilerGLSL::Options::Highp;
        comp.set_common_options(opts);

        auto res = comp.get_shader_resources();
        auto unbind = [&](const spirv_cross::SmallVector<spirv_cross::Resource> &list) {
            for (auto &r : list) {
                comp.unset_decoration(r.id, spv::DecorationBinding);
                comp.unset_decoration(r.id, spv::DecorationDescriptorSet);
            }
        };
        unbind(res.uniform_buffers);
        unbind(res.storage_buffers);
        unbind(res.storage_images);
        unbind(res.sampled_images);
        unbind(res.separate_images);
        unbind(res.separate_samplers);
        if (comp.get_execution_model() == spv::ExecutionModelVertex)
            for (auto &r : res.stage_inputs)
                comp.unset_decoration(r.id, spv::DecorationLocation);

        spirv_cross::VariableID dummy = comp.build_dummy_sampler_for_combined_images();
        if (dummy != 0) {
            comp.unset_decoration(dummy, spv::DecorationBinding);
            comp.unset_decoration(dummy, spv::DecorationDescriptorSet);
        }
        comp.build_combined_image_samplers();
        std::map<uint32_t, int> uses;      // image id -> combined samplers so far
        for (auto &c : comp.get_combined_image_samplers()) {
            std::string name = comp.get_name(c.image_id);
            if (name.empty())
                name = "_" + std::to_string(uint32_t(c.image_id));
            int k = uses[uint32_t(c.image_id)]++;
            if (k)
                name += "_PMSAMPLER" + std::to_string(k);
            comp.set_name(c.combined_id, name);
        }
        out = comp.compile();
    } catch (const std::exception &ex) {
        log += std::string("error: ") + ex.what() + "\n";
        return false;
    }
    return true;
}

bool read_file(const char *path, std::string &out)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return false;
    char buf[65536];
    size_t n;
    out.clear();
    while ((n = fread(buf, 1, sizeof buf, f)) > 0)
        out.append(buf, n);
    fclose(f);
    return true;
}

int serve()
{
    char line[256];
    while (fgets(line, sizeof line, stdin)) {
        char op = 0;
        int arg = 0;
        long len = 0;
        if (sscanf(line, "%c %d %ld", &op, &arg, &len) < 1 || op == 'q')
            break;
        if (len < 0 || len > (64L << 20))
            return 1;
        std::string in(len, '\0');
        if (len && fread(&in[0], 1, len, stdin) != (size_t)len)
            return 1;
        std::string out, log;
        bool ok = false;
        if (op == 'd')
            ok = decode(in, out, log, arg == 1);
        else if (op == 'c')
            ok = translate(in, arg ? arg : 300, out, log);
        else
            log = "error: unknown request\n";
        if (!ok)
            out.clear();
        printf("%d %zu %zu\n", ok ? 0 : 1, out.size(), log.size());
        fwrite(out.data(), 1, out.size(), stdout);
        fwrite(log.data(), 1, log.size(), stdout);
        fflush(stdout);
    }
    return 0;
}

} // namespace

int main(int argc, char **argv)
{
    if (argc == 2 && !strcmp(argv[1], "-s"))
        return serve();
    if (argc == 4 && !strcmp(argv[1], "-d")) {
        std::string in, out, log;
        if (!read_file(argv[2], in) || !decode(in, out, log)) {
            fprintf(stderr, "%s: %s", argv[2], log.empty() ? "cannot read\n" : log.c_str());
            return 1;
        }
        FILE *f = fopen(argv[3], "wb");
        if (!f || fwrite(out.data(), 1, out.size(), f) != out.size())
            return 1;
        fclose(f);
        return 0;
    }
    int es = 300, first = 1;
    if (argc > 2 && !strcmp(argv[1], "-e")) {
        es = atoi(argv[2]);
        first = 3;
    }
    if (first >= argc) {
        fprintf(stderr, "usage: pmspirvcross [-e 300|310] <stage>... | -d <in.smolv> <out.spv> | -s\n");
        return 2;
    }
    int status = 0;
    for (int i = first; i < argc; i++) {
        std::string in, out, log;
        if (!read_file(argv[i], in) || !translate(in, es, out, log)) {
            fprintf(stderr, "%s: %s", argv[i], log.empty() ? "cannot read\n" : log.c_str());
            status = 1;
            continue;
        }
        printf("%s", out.c_str());
    }
    return status;
}
