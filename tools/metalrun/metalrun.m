// metalrun <kernel.metal> <tg x,y,z> <groups x,y,z> <index>=<file>...: compiles the MSL (fast math
// off), binds each file's bytes as the buffer at that Metal index, dispatches, and writes every
// buffer back to <file>.out. The porter's Mac only: a reference run of a kernel whose GLSL
// translation computecheck runs on the same files (docs/shaders.md, "Metal kernels"). Objective-C:
// Metal's API is Apple's. Build on the Mac:
//   clang -fobjc-arc -O2 -framework Foundation -framework Metal -o metalrun metalrun.m
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

int main(int argc, char **argv)
{
    @autoreleasepool {
        if (argc < 5) { fprintf(stderr, "usage: metalrun <kernel.metal> <tg x,y,z> <groups x,y,z> <index>=<file>...\n"); return 2; }
        NSError *err = nil;
        NSString *src = [NSString stringWithContentsOfFile:@(argv[1]) encoding:NSUTF8StringEncoding error:&err];
        if (!src) { fprintf(stderr, "metalrun: cannot read %s\n", argv[1]); return 2; }
        unsigned tg[3], gr[3];
        if (sscanf(argv[2], "%u,%u,%u", &tg[0], &tg[1], &tg[2]) != 3 || sscanf(argv[3], "%u,%u,%u", &gr[0], &gr[1], &gr[2]) != 3) {
            fprintf(stderr, "metalrun: sizes are x,y,z\n"); return 2;
        }
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        MTLCompileOptions *opt = [MTLCompileOptions new];
        if (@available(macOS 15.0, *)) opt.mathMode = MTLMathModeSafe;
        else {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
            opt.fastMathEnabled = NO;    // (macOS 14 and older)
#pragma clang diagnostic pop
        }
        id<MTLLibrary> lib = [dev newLibraryWithSource:src options:opt error:&err];
        if (!lib) { fprintf(stderr, "metalrun: %s\n", err.localizedDescription.UTF8String); return 1; }
        id<MTLFunction> fn = [lib newFunctionWithName:@"computeMain"];
        id<MTLComputePipelineState> ps = [dev newComputePipelineStateWithFunction:fn error:&err];
        if (!ps) { fprintf(stderr, "metalrun: %s\n", err.localizedDescription.UTF8String); return 1; }
        id<MTLCommandQueue> q = [dev newCommandQueue];
        id<MTLCommandBuffer> cb = [q commandBuffer];
        id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
        [enc setComputePipelineState:ps];
        NSMutableArray *bufs = [NSMutableArray array], *paths = [NSMutableArray array];
        for (int i = 4; i < argc; i++) {
            char *eq = strchr(argv[i], '=');
            if (!eq) { fprintf(stderr, "metalrun: %s is not <index>=<file>\n", argv[i]); return 2; }
            int idx = atoi(argv[i]);
            NSData *d = [NSData dataWithContentsOfFile:@(eq + 1)];
            if (!d) { fprintf(stderr, "metalrun: cannot read %s\n", eq + 1); return 2; }
            id<MTLBuffer> b = [dev newBufferWithBytes:d.bytes length:d.length options:MTLResourceStorageModeShared];
            [enc setBuffer:b offset:0 atIndex:idx];
            [bufs addObject:b]; [paths addObject:@(eq + 1)];
        }
        [enc dispatchThreadgroups:MTLSizeMake(gr[0], gr[1], gr[2]) threadsPerThreadgroup:MTLSizeMake(tg[0], tg[1], tg[2])];
        [enc endEncoding];
        [cb commit];
        [cb waitUntilCompleted];
        if (cb.error) { fprintf(stderr, "metalrun: %s\n", cb.error.localizedDescription.UTF8String); return 1; }
        for (NSUInteger i = 0; i < bufs.count; i++) {
            id<MTLBuffer> b = bufs[i];
            [[NSData dataWithBytes:b.contents length:b.length] writeToFile:[paths[i] stringByAppendingString:@".out"] atomically:YES];
        }
    }
    return 0;
}
