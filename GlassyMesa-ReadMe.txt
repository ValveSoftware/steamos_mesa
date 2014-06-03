
LunarGlass + LLVM on Mesa Implementation Notes

RELEASE NOTES

Note that this is a work in progress.  Some functionality is not
implemented or working.  A subset of the presently unimplemented
functionality:

  * IO blocks
  * uniform initializers
  * texture rectangle sampling
  * varying arrays
  * some loop related functionality
  * several extensions, such as:
    * GL_ARB_shader_texture_lod
    * GL_ARB_fragment_coord_conventions

BUILDING
  1. Install and build glslang and LunarGlass as in the link below, but during 
    "Step 3: Build LLVM 3.4", you MUST add the REQUIRES_RTTI flag as follows:

     REQUIRES_RTTI=1 make -j $(nproc) && make install DESTDIR=`pwd`/install

     https://code.google.com/p/lunarglass/wiki/Building

  2. Checkout mesa and drm as siblings of the glslang and LunarGlass
     directories. They should live along side, like so:

    drwxr-xr-x 18 user user 4096 Mar 14 08:26 drm/
    drwxr-xr-x 10 user user 4096 Apr 23 13:52 glslang/
    drwxr-xr-x 12 user user 4096 Apr 23 14:33 LunarGLASS/
    drwxr-xr-x 12 user user 4096 Apr 23 09:53 mesa/

    Ensure that you don't accidentally have a "mesa/mesa"
    structure. You should have a "autogen.sh" directly under the mesa
    directory.

    Fetch mesa like so, which will create a new directory called mesa
    and populate it:

    git clone ssh://apollo/~git/Escape/mesa mesa

    Checkout a branch that you want to try, for instance:

    git checkout -t origin/2014ww22.3_i965-LunarGlass

    At the time of this writing, the latest branch is as above. I'll
    try to keep this updated, but there's considerable risk of
    text-rot.

  3. Configure mesa as follows. Replace YOUR/PATH/TO/BUILD with the
     directory from "2" above which contains LunarGlass?. (TODO: maybe
     not necessary any more; I made some improvements to autoconf so it
     might work without being pointed to explicitly for the structure
     from 2 above, but they're untested).

    Make sure you fully expand the path to LLVM; don't use a relative path.

./autogen.sh --enable-opengl --enable-gles1 --enable-gles2 --enable-egl --enable-dri --enable-glx --enable-shared-glapi --enable-glx-tls --enable-texture-float --enable-dri3=no --with-dri-drivers=i965,swrast --with-gallium-drivers=swrast --enable-debug --with-llvm-prefix=/YOUR/PATH/TO/BUILD/LunarGLASS/Core/LLVM/llvm-3.4/build/install/usr/local --disable-llvm-shared-libs --enable-gallium-llvm --enable-lunarglass

    In particular, you must provide the --enable-lunarglass and --disable-llvm-shared-libs switches.

  4. Build mesa: 

     make -j $(nproc)

  5.  Running Standalone 

    cd mesa/src/glsl
   ./glsl_compiler --dump-hir shader.{vert|frag}

  6.  Running the driver 

    You can run the driver stack as any normal Mesa, by pointing
    LD_LIBRARY_PATH and LIBGL_DRIVERS_PATH environment variables to
    your mesa/lib directory.


