/* Host-side validation with the same effect parser and GLSL ES translator used
 * by FNA3D. No graphics context or proprietary input is needed. */
#include "mojoshader.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int shader_errors;
static int shader_count;

static void *MOJOSHADERCALL compile_shader(
    const void *ctx, const char *name, const unsigned char *bytes,
    unsigned int length, const MOJOSHADER_swizzle *swizzles,
    unsigned int swizzle_count, const MOJOSHADER_samplerMap *samplers,
    unsigned int sampler_count)
{
    const MOJOSHADER_parseData *data = MOJOSHADER_parse(
        MOJOSHADER_PROFILE_GLSLES3, name, bytes, length,
        swizzles, swizzle_count, samplers, sampler_count, NULL, NULL, NULL);
    shader_count++;
    if (data == NULL) {
        shader_errors++;
        return NULL;
    }
    for (int i = 0; i < data->error_count; ++i) {
        fprintf(stderr, "%s: %s\n", name, data->errors[i].error);
        shader_errors++;
    }
    if (data->output != NULL && strstr(data->output, "uniform sampler3D ") != NULL &&
        strstr(data->output, "precision lowp sampler3D;") == NULL) {
        fprintf(stderr, "%s: volume sampler has no GLSL ES precision\n", name);
        shader_errors++;
    }
    if (ctx != NULL && data->error_count == 0) {
        char path[4096];
        int count = snprintf(path, sizeof(path), "%s/%d.%s", (const char *) ctx,
            shader_count, data->shader_type == MOJOSHADER_TYPE_VERTEX ? "vert" : "frag");
        FILE *file = count > 0 && (size_t) count < sizeof(path) ? fopen(path, "wb") : NULL;
        if (file == NULL) {
            fprintf(stderr, "%s: could not write translated shader\n", name);
            shader_errors++;
        } else {
            if (fwrite(data->output, 1, (size_t) data->output_len, file) != (size_t) data->output_len)
                shader_errors++;
            if (fclose(file) != 0) shader_errors++;
        }
    }
    return (void *) data;
}

static void MOJOSHADERCALL addref(void *shader) { (void) shader; }
static void MOJOSHADERCALL delete_shader(const void *ctx, void *shader)
{
    (void) ctx;
    MOJOSHADER_freeParseData((const MOJOSHADER_parseData *) shader);
}
static MOJOSHADER_parseData *MOJOSHADERCALL parse_data(void *shader)
{
    return (MOJOSHADER_parseData *) shader;
}
static void MOJOSHADERCALL bind(const void *ctx, void *vs, void *ps)
{
    (void) ctx; (void) vs; (void) ps;
}
static void MOJOSHADERCALL bound(const void *ctx, void **vs, void **ps)
{
    (void) ctx; *vs = NULL; *ps = NULL;
}
static void MOJOSHADERCALL map(const void *ctx, float **vf, int **vi,
    unsigned char **vb, float **pf, int **pi, unsigned char **pb)
{
    (void) ctx; (void) vf; (void) vi; (void) vb;
    (void) pf; (void) pi; (void) pb;
}
static void MOJOSHADERCALL unmap(const void *ctx) { (void) ctx; }
static const char *MOJOSHADERCALL error(const void *ctx)
{
    (void) ctx;
    return "GLSL ES translation failed";
}

int main(int argc, char **argv)
{
    if (argc != 2 && argc != 3) {
        fprintf(stderr, "usage: cmz-effect-check <project-authored.fxb> [shader-output-directory]\n");
        return 2;
    }
    FILE *file = fopen(argv[1], "rb");
    if (file == NULL) { perror(argv[1]); return 1; }
    if (fseek(file, 0, SEEK_END) != 0) { fclose(file); return 1; }
    long length = ftell(file);
    if (length < 8 || length > 16 * 1024 * 1024 || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file); return 1;
    }
    unsigned char *bytes = (unsigned char *) malloc((size_t) length);
    if (bytes == NULL) { fclose(file); return 1; }
    size_t read = fread(bytes, 1, (size_t) length, file);
    fclose(file);
    if (read != (size_t) length) { free(bytes); return 1; }

    MOJOSHADER_effectShaderContext ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.shaderContext = argc == 3 ? argv[2] : NULL;
    ctx.compileShader = compile_shader;
    ctx.shaderAddRef = addref;
    ctx.deleteShader = delete_shader;
    ctx.getParseData = parse_data;
    ctx.bindShaders = bind;
    ctx.getBoundShaders = bound;
    ctx.mapUniformBufferMemory = map;
    ctx.unmapUniformBufferMemory = unmap;
    ctx.getError = error;
    MOJOSHADER_effect *effect = MOJOSHADER_compileEffect(
        bytes, (unsigned int) length, NULL, 0, NULL, 0, &ctx);
    free(bytes);
    if (effect == NULL) return 1;
    for (int i = 0; i < effect->error_count; ++i)
        fprintf(stderr, "%s\n", effect->errors[i].error);

    printf("effect errors=%d shader_errors=%d shaders=%d parameters=%d techniques=%d\n",
        effect->error_count, shader_errors, shader_count,
        effect->param_count, effect->technique_count);
    for (int i = 0; i < effect->param_count; ++i) {
        const MOJOSHADER_effectValue *v = &effect->params[i].value;
        printf("parameter %s type=%d class=%d rows=%u columns=%u elements=%u semantic=%s\n",
            v->name, (int) v->type.parameter_type, (int) v->type.parameter_class,
            v->type.rows, v->type.columns, v->type.elements, v->semantic ? v->semantic : "");
    }
    for (int i = 0; i < effect->technique_count; ++i) {
        const MOJOSHADER_effectTechnique *t = &effect->techniques[i];
        printf("technique %s", t->name);
        for (unsigned int j = 0; j < t->pass_count; ++j)
            printf(" |%s", t->passes[j].name ? t->passes[j].name : "");
        putchar('\n');
    }
    int failed = effect->error_count != 0 || shader_errors != 0 || shader_count == 0;
    MOJOSHADER_deleteEffect(effect);
    return failed;
}
