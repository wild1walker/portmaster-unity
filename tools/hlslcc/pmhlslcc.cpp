// pmhlslcc: Direct3D 11 shader bytecode (DXBC) -> GLSL with Unity's HLSLcc, the translator Unity
// itself uses for its OpenGL programs. The converter (convert/pmunity/shaders/dxbc.py) runs it to
// give a Windows build's D3D11-only shaders GL programs. build.sh fetches HLSLcc and builds this.
//
//   pmhlslcc [-l LANG] [-f FLAGS] <stage.dxbc>...    translate one program (its stages, any order)
//                                                    and print it as Unity stores GL programs:
//                                                    #ifdef VERTEX / FRAGMENT blocks
//   pmhlslcc -s                                      serve requests on stdin (below), for many programs
//
// LANG: 150 (default, what Unity 2017-2018 writes for OpenGL Core), 330, 410, 430, es300, es310.
// FLAGS: HLSLCC_FLAG_* bits (include/hlslcc.h), default 0x20800 as Unity's GL programs are written:
// TRANSLATE_MATRICES (matrices as vec4 arrays named hlslcc_mtx4x4<name>) and DISABLE_GLOBALS_STRUCT
// (every constant buffer member a plain uniform).
//
// The stages of one program share one GLSLCrossDependencyData (varying names and locations,
// interpolation modes) and are translated pixel shader first, as HLSLcc requires.
//
// Serve protocol (binary-safe, one request at a time):
//   request  "<n> <flags> <lang>" then n x " <length>", '\n', then the n DXBC blobs
//   answer   "<status> <n>" then n x " <length>", " <log length>\n", the n GLSL texts (request
//            order), then the log (diagnostics, one per line). status 0: translated; 1: failed.
// A request with n = 0 ends the server.
#include "hlslcc.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

struct Reflection : HLSLccReflection {
    std::string log;
    void OnDiagnostics(const std::string &error, int line, bool isError) override
    {
        log += isError ? "error: " : "warning: ";
        if (line >= 0)
            log += "line " + std::to_string(line) + ": ";
        log += error + "\n";
    }
};

bool parse_lang(const char *s, GLLang *out)
{
    static const struct { const char *name; GLLang lang; } langs[] = {
        {"150", LANG_150}, {"330", LANG_330}, {"410", LANG_410}, {"430", LANG_430},
        {"es300", LANG_ES_300}, {"es310", LANG_ES_310},
    };
    for (const auto &l : langs)
        if (!strcmp(s, l.name)) { *out = l.lang; return true; }
    return false;
}

// The shader type of a DXBC container (SHADER_TYPE), or INVALID_SHADER if it is not one we can
// hand to HLSLcc (checked here so a damaged blob is reported rather than read out of bounds).
SHADER_TYPE dxbc_type(const std::string &b)
{
    auto u32 = [&](size_t off) { uint32_t v; memcpy(&v, b.data() + off, 4); return v; };
    if (b.size() < 32 || b.compare(0, 4, "DXBC") != 0 || u32(24) > b.size())
        return INVALID_SHADER;
    uint32_t chunks = u32(28);
    if (chunks > 64 || 32 + 4 * (size_t)chunks > b.size())
        return INVALID_SHADER;
    for (uint32_t i = 0; i < chunks; i++) {
        size_t off = u32(32 + 4 * i);
        if (off + 12 > b.size() || off + 8 + (size_t)u32(off + 4) > b.size())
            return INVALID_SHADER;
        if (!b.compare(off, 4, "SHDR") || !b.compare(off, 4, "SHEX")) {
            switch (u32(off + 8) >> 16) {          // program type in the version token
                case 0: return PIXEL_SHADER;
                case 1: return VERTEX_SHADER;
                case 2: return GEOMETRY_SHADER;
                case 3: return HULL_SHADER;
                case 4: return DOMAIN_SHADER;
                case 5: return COMPUTE_SHADER;
                default: return INVALID_SHADER;
            }
        }
    }
    return INVALID_SHADER;
}

const char *stage_name(SHADER_TYPE t)
{
    switch (t) {
        case VERTEX_SHADER: return "VERTEX";
        case PIXEL_SHADER: return "FRAGMENT";
        case GEOMETRY_SHADER: return "GEOMETRY";
        case HULL_SHADER: return "HULL";
        case DOMAIN_SHADER: return "DOMAIN";
        default: return "COMPUTE";
    }
}

// Translates the stages of one program. glsl gets one text per stage (request order).
bool translate(const std::vector<std::string> &stages, unsigned flags, GLLang lang,
               std::vector<std::string> &glsl, std::string &log)
{
    static const SHADER_TYPE order[] = {PIXEL_SHADER, HULL_SHADER, DOMAIN_SHADER, GEOMETRY_SHADER, VERTEX_SHADER, COMPUTE_SHADER};
    static const uint32_t stage_flag[] = {PS_FLAG_PIXEL_SHADER, PS_FLAG_VERTEX_SHADER, PS_FLAG_GEOMETRY_SHADER,
                                          PS_FLAG_HULL_SHADER, PS_FLAG_DOMAIN_SHADER, 0};
    std::vector<SHADER_TYPE> types;
    GLSLCrossDependencyData deps;
    deps.SetupGLSLResourceBindingSlotsIndices();
    for (size_t i = 0; i < stages.size(); i++) {
        SHADER_TYPE t = dxbc_type(stages[i]);
        if (t == INVALID_SHADER) {
            log += "error: stage " + std::to_string(i) + ": not a valid DXBC shader\n";
            return false;
        }
        for (size_t j = 0; j < i; j++)
            if (types[j] == t) {
                log += std::string("error: two ") + stage_name(t) + " stages\n";
                return false;
            }
        types.push_back(t);
        deps.ui32ProgramStages |= stage_flag[t];
    }
    glsl.assign(stages.size(), std::string());
    GlExtensions ext = {};
    ext.ARB_explicit_attrib_location = lang == LANG_150;
    bool ok = true;
    for (SHADER_TYPE t : order)
        for (size_t i = 0; i < stages.size(); i++) {
            if (types[i] != t)
                continue;
            // HLSLcc reads the container as 32-bit words: hand it an aligned copy
            std::vector<uint32_t> words((stages[i].size() + 3) / 4);
            memcpy(words.data(), stages[i].data(), stages[i].size());
            Reflection refl;
            HLSLccSamplerPrecisionInfo precisions;
            GLSLShader out;
            if (!TranslateHLSLFromMem((const char *)words.data(), flags, lang, &ext, &deps, precisions, refl, &out)) {
                refl.log += std::string("error: ") + stage_name(t) + ": HLSLcc could not translate this stage\n";
                ok = false;
            }
            glsl[i] = out.sourceCode;
            for (size_t p = 0; p < refl.log.size();) {   // prefix each diagnostic with its stage
                size_t e = refl.log.find('\n', p);
                log += std::string(stage_name(t)) + ": " + refl.log.substr(p, e - p) + "\n";
                p = e + 1;
            }
        }
    return ok;
}

bool read_exact(FILE *f, std::string &s, size_t n)
{
    s.resize(n);
    return n == 0 || fread(&s[0], 1, n, f) == n;
}

int serve()
{
    char line[1024];
    while (fgets(line, sizeof line, stdin)) {
        char *p = line;
        long n = strtol(p, &p, 10);
        unsigned long flags = strtoul(p, &p, 0);
        char lang_s[16] = "";
        int used = 0;
        if (n <= 0 || n > 6 || sscanf(p, " %15s%n", lang_s, &used) != 1)
            return n == 0 ? 0 : 2;
        p += used;
        std::vector<size_t> lens;
        for (long i = 0; i < n; i++)
            lens.push_back(strtoul(p, &p, 10));
        std::vector<std::string> stages(n);
        for (long i = 0; i < n; i++)
            if (!read_exact(stdin, stages[i], lens[i]))
                return 2;
        GLLang lang;
        std::vector<std::string> glsl(n);
        std::string log;
        bool ok = parse_lang(lang_s, &lang);
        if (!ok)
            log = std::string("error: unknown language ") + lang_s + "\n";
        else
            ok = translate(stages, (unsigned)flags, lang, glsl, log);
        printf("%d %ld", ok ? 0 : 1, n);
        for (const auto &g : glsl)
            printf(" %zu", g.size());
        printf(" %zu\n", log.size());
        for (const auto &g : glsl)
            fwrite(g.data(), 1, g.size(), stdout);
        fwrite(log.data(), 1, log.size(), stdout);
        fflush(stdout);
    }
    return 0;
}

}  // namespace

int main(int argc, char **argv)
{
    GLLang lang = LANG_150;
    unsigned flags = HLSLCC_FLAG_TRANSLATE_MATRICES | HLSLCC_FLAG_DISABLE_GLOBALS_STRUCT;
    std::vector<std::string> stages;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-s"))
            return serve();
        if (!strcmp(argv[i], "-l") && i + 1 < argc) {
            if (!parse_lang(argv[++i], &lang)) {
                fprintf(stderr, "unknown language %s\n", argv[i]);
                return 2;
            }
        } else if (!strcmp(argv[i], "-f") && i + 1 < argc) {
            flags = (unsigned)strtoul(argv[++i], NULL, 0);
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "usage: pmhlslcc [-l LANG] [-f FLAGS] <stage.dxbc>... | pmhlslcc -s\n");
            return 2;
        } else {
            FILE *f = fopen(argv[i], "rb");
            if (!f) { perror(argv[i]); return 2; }
            std::string s;
            char buf[65536];
            size_t k;
            while ((k = fread(buf, 1, sizeof buf, f)) > 0)
                s.append(buf, k);
            fclose(f);
            stages.push_back(s);
        }
    }
    if (stages.empty()) {
        fprintf(stderr, "usage: pmhlslcc [-l LANG] [-f FLAGS] <stage.dxbc>... | pmhlslcc -s\n");
        return 2;
    }
    std::vector<std::string> glsl;
    std::string log;
    bool ok = translate(stages, flags, lang, glsl, log);
    fputs(log.c_str(), stderr);
    if (!ok)
        return 1;
    static const SHADER_TYPE print_order[] = {VERTEX_SHADER, HULL_SHADER, DOMAIN_SHADER, GEOMETRY_SHADER, PIXEL_SHADER, COMPUTE_SHADER};
    for (SHADER_TYPE t : print_order)
        for (size_t i = 0; i < stages.size(); i++)
            if (dxbc_type(stages[i]) == t)
                printf("#ifdef %s\n%s\n#endif\n", stage_name(t), glsl[i].c_str());
    return 0;
}
