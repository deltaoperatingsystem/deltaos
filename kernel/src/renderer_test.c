/* engine.c - pure C99 version of the 3D engine.
 *
 * Assumes platform functions (from your kernel):
 *   int fb_width(void);
 *   int fb_height(void);
 *   void fb_clear(uint32 color);
 *   void fb_putpixel(int x, int y, uint32 color);
 *   void fb_drawline(int x1,int y1,int x2,int y2,uint32 color);
 *   int get_keystate(int key);   // non-zero if pressed
 *   void con_flush(void);
 *   void sleep(unsigned ms);
 *
 * If your framebuffer API uses different names or color formats, change the fb_* calls.
 */


#include <lib/math.h>
#include <drivers/fb.h>
#include <drivers/keyboard.h>
#include <drivers/console.h>
#include <drivers/vt/vt.h>
#include <lib/time.h>
#include <lib/mem.h>
#include <lib/string.h>

#define MIN(a,b) ((a)<(b)?(a):(b))
#define MAX(a,b) ((a)>(b)?(a):(b))

/* ENGINE CONFIG */
static const double PI = 3.14159265358979323846;
static const unsigned TARGET_FPS = 60;
static const double CAMERA_FOV_DEG = 70.0;
static const double CAMERA_NEAR = 0.1;
static const double CAMERA_FAR  = 100.0;

/* -------------------------------------------------------------------------- */
/* Basic math: vec3, vec4, mat4 (column-major)                                */
/* -------------------------------------------------------------------------- */

typedef struct { double x,y,z; } vec3;
typedef struct { double x,y,z,w; } vec4;

static inline vec3 v3(double x,double y,double z){ return (vec3){x,y,z}; }
static inline vec4 v4(double x,double y,double z,double w){ return (vec4){x,y,z,w}; }

static inline vec3 v3_add(vec3 a, vec3 b){ return (vec3){a.x+b.x,a.y+b.y,a.z+b.z}; }
static inline vec3 v3_sub(vec3 a, vec3 b){ return (vec3){a.x-b.x,a.y-b.y,a.z-b.z}; }
static inline vec3 v3_mul(vec3 a, double s){ return (vec3){a.x*s,a.y*s,a.z*s}; }
static inline double v3_dot(vec3 a, vec3 b){ return a.x*b.x + a.y*b.y + a.z*b.z; }
static inline vec3 v3_cross(vec3 a, vec3 b){
    return (vec3){ a.y*b.z - a.z*b.y,
                   a.z*b.x - a.x*b.z,
                   a.x*b.y - a.y*b.x };
}
static inline double v3_len(vec3 a){ return sqrt(v3_dot(a,a)); }
static inline vec3 v3_norm(vec3 a){
    double l = v3_len(a);
    if (l == 0.0) return a;
    return v3_mul(a, 1.0 / l);
}

/* 4x4 matrix as array[16] column-major for readability with math */
typedef double mat4[16];

static void mat4_identity(mat4 out){
    memset(out, 0, sizeof(mat4));
    out[0]=out[5]=out[10]=out[15]=1.0;
}

/* out = a * b  (both column-major) */
static void mat4_mul(mat4 out, const mat4 a, const mat4 b){
    mat4 r;
    for(int c=0;c<4;c++){
        for(int rrow=0;rrow<4;rrow++){
            double sum = 0.0;
            for(int k=0;k<4;k++){
                /* a element at row=rrow col=k is a[k*4 + rrow]
                   b element at row=k col=c is b[c*4 + k]
                   result at row=rrow col=c is r[c*4 + rrow] */
                sum += a[k*4 + rrow] * b[c*4 + k];
            }
            r[c*4 + rrow] = sum;
        }
    }
    memcpy(out, r, sizeof(mat4));
}

static void mat4_translate(mat4 out, double tx,double ty,double tz){
    mat4_identity(out);
    out[12]=tx; out[13]=ty; out[14]=tz;
}

static void mat4_rotation_y(mat4 out, double theta){
    double c = cos(theta), s = sin(theta);
    mat4_identity(out);
    out[0] = c; out[8] = s;
    out[2] = -s; out[10] = c;
}

static void mat4_rotation_x(mat4 out, double theta){
    double c = cos(theta), s = sin(theta);
    mat4_identity(out);
    out[5] = c; out[9] = -s;
    out[6] = s; out[10] = c;
}

/* build a simple look-at view matrix (right-handed, camera looking -Z) */
static void mat4_look_at(mat4 out, vec3 eye, vec3 center, vec3 up){
    vec3 f = v3_norm(v3_sub(center, eye));
    vec3 s = v3_norm(v3_cross(f, up));
    vec3 u = v3_cross(s, f);

    /* column-major */
    out[0]=s.x; out[1]=u.x; out[2]=-f.x; out[3]=0.0;
    out[4]=s.y; out[5]=u.y; out[6]=-f.y; out[7]=0.0;
    out[8]=s.z; out[9]=u.z; out[10]=-f.z; out[11]=0.0;
    out[12]=-v3_dot(s,eye); out[13]=-v3_dot(u,eye); out[14]=v3_dot(f,eye); out[15]=1.0;
}

/* perspective projection matrix (right-handed, NDC z in [-1,1]) */
static void mat4_perspective(mat4 out, double fov_deg, double aspect, double znear, double zfar){
    double fov = fov_deg * (PI/180.0);
    double f = 1.0 / tan(fov / 2.0);
    memset(out, 0, sizeof(mat4));
    out[0] = f / aspect;
    out[5] = f;
    out[10] = (zfar + znear) / (znear - zfar);
    out[11] = -1.0;
    out[14] = (2.0 * zfar * znear) / (znear - zfar);
}

/* multiply mat4 by vec4 (column-major) */
static vec4 mat4_mul_vec4(const mat4 m, vec4 v){
    vec4 r;
    r.x = m[0]*v.x + m[4]*v.y + m[8]*v.z + m[12]*v.w;
    r.y = m[1]*v.x + m[5]*v.y + m[9]*v.z + m[13]*v.w;
    r.z = m[2]*v.x + m[6]*v.y + m[10]*v.z + m[14]*v.w;
    r.w = m[3]*v.x + m[7]*v.y + m[11]*v.z + m[15]*v.w;
    return r;
}

/* viewport transform: from NDC (-1..1) to screen coords */
static void ndc_to_screen(double ndc_x, double ndc_y, int *sx, int *sy){
    int w = fb_width();
    int h = fb_height();
    /* keep full screen, origin top-left */
    double x = (ndc_x + 1.0) * 0.5 * (double)w;
    double y = (1.0 - (ndc_y + 1.0) * 0.5) * (double)h;
    int xi = (int) floor(x + 0.5);
    int yi = (int) floor(y + 0.5);
    if (xi < 0) xi = 0;
    if (xi >= w) xi = w-1;
    if (yi < 0) yi = 0;
    if (yi >= h) yi = h-1;
    *sx = xi; *sy = yi;
}

/* pack grayscale into platform color format - assumes 0xRRGGBB */
static inline uint32 color_from_intensity(double t){
    if (t < 0.0) t = 0.0;
    if (t > 1.0) t = 1.0;
    uint8 v = (uint8) (t * 255.0);
    return (uint32)((v<<16) | (v<<8) | v);
}

/* -------------------------------------------------------------------------- */
/* Mesh, camera, and renderer state                                           */
/* -------------------------------------------------------------------------- */

typedef struct {
    vec3 pos;
} Vertex;

typedef struct {
    int a,b,c; /* indices into vertex array (triangles) */
    vec3 normal; /* computed per-face (model space) */
} Tri;

typedef struct {
    Vertex *verts;
    size vert_count;
    Tri *tris;
    size tri_count;
} Mesh;

/* simple cube mesh factory */
static Mesh make_cube(double half_size){
    /* 8 verts, 12 tris */
    Mesh m;
    m.vert_count = 8;
    m.tri_count = 12;
    m.verts = (Vertex*)malloc(sizeof(Vertex) * m.vert_count);
    m.tris  = (Tri*)malloc(sizeof(Tri) * m.tri_count);
    double s = half_size;
    m.verts[0].pos = v3( s,  s,  s);
    m.verts[1].pos = v3(-s,  s,  s);
    m.verts[2].pos = v3(-s, -s,  s);
    m.verts[3].pos = v3( s, -s,  s);
    m.verts[4].pos = v3( s,  s, -s);
    m.verts[5].pos = v3(-s,  s, -s);
    m.verts[6].pos = v3(-s, -s, -s);
    m.verts[7].pos = v3( s, -s, -s);

    /* front face (0,1,2,3), back face (4,5,6,7), etc. two tris per face */
    int tri_idx = 0;
    #define ADDTRI(i,j,k) do { m.tris[tri_idx].a=(i); m.tris[tri_idx].b=(j); m.tris[tri_idx].c=(k); tri_idx++; } while(0)
    ADDTRI(0,1,2); ADDTRI(0,2,3); /* front */
    ADDTRI(4,7,6); ADDTRI(4,6,5); /* back */
    ADDTRI(0,4,5); ADDTRI(0,5,1); /* top */
    ADDTRI(3,2,6); ADDTRI(3,6,7); /* bottom */
    ADDTRI(1,5,6); ADDTRI(1,6,2); /* left */
    ADDTRI(0,3,7); ADDTRI(0,7,4); /* right */
    #undef ADDTRI

    /* compute face normals in model space (we'll transform to world/camera as needed) */
    for (size i=0;i<m.tri_count;i++){
        vec3 a = m.verts[m.tris[i].a].pos;
        vec3 b = m.verts[m.tris[i].b].pos;
        vec3 c = m.verts[m.tris[i].c].pos;
        vec3 n = v3_cross(v3_sub(b,a), v3_sub(c,a));
        m.tris[i].normal = v3_norm(n);
    }
    return m;
}

/* free mesh resources */
static void free_mesh(Mesh *m){
    if (!m) return;
    free(m->verts); free(m->tris);
    m->verts = NULL; m->tris = NULL;
    m->vert_count = m->tri_count = 0;
}

/* Renderer z-buffer */
static float *zbuffer = NULL;
static int zbuf_width = 0, zbuf_height = 0;

static void ensure_zbuffer_size(int w,int h){
    if (zbuffer && zbuf_width==w && zbuf_height==h) return;
    free(zbuffer);
    zbuffer = (float*)malloc(sizeof(float) * (size)w * (size)h);
    if (!zbuffer) { zbuf_width=zbuf_height=0; return; }
    zbuf_width = w; zbuf_height = h;
}

/* clear zbuffer with very far value */
static void clear_zbuffer(void){
    if (!zbuffer) return;
    size n = (size)zbuf_width * (size)zbuf_height;
    for (size i=0;i<n;i++) zbuffer[i] = (float)1e30f;
}

/* write pixel with depth test: depth here is positive camera-space z (smaller = closer) */
static inline void put_pixel_depth(int x,int y,double depth,double intensity,uint32 colour){
    if (!zbuffer) return;
    if (x<0 || x>=zbuf_width || y<0 || y>=zbuf_height) return;
    size idx = (size)y * (size)zbuf_width + (size)x;
    if (depth >= (double)zbuffer[idx]) return;
    zbuffer[idx] = (float)depth;
    uint32 col = color_from_intensity(intensity) & colour;
    fb_putpixel(x,y,col);
}

/* -------------------------------------------------------------------------- */
/* Triangle rasterizer using barycentric coordinates                          */
/* -------------------------------------------------------------------------- */

typedef struct {
    double x,y;  /* screen coords */
    double ndc_x, ndc_y; /* ndc coords (for interpolation if needed) */
    double cam_z; /* camera-space Z (positive forward) */
} ScreenVert;

static inline double edge_fn(double ax,double ay,double bx,double by,double cx,double cy){
    return (cx - ax) * (by - ay) - (cy - ay) * (bx - ax);
}

static void rasterize_triangle(ScreenVert v0, ScreenVert v1, ScreenVert v2, double intensity){
    /* bounding box in integer screen coordinates */
    int minx = (int) floor(MIN(MIN(v0.x, v1.x), v2.x));
    int maxx = (int) ceil (MAX(MAX(v0.x, v1.x), v2.x));
    int miny = (int) floor(MIN(MIN(v0.y, v1.y), v2.y));
    int maxy = (int) ceil (MAX(MAX(v0.y, v1.y), v2.y));

    /* clip to screen */
    if (minx < 0) minx = 0;
    if (miny < 0) miny = 0;
    if (maxx >= zbuf_width) maxx = zbuf_width - 1;
    if (maxy >= zbuf_height) maxy = zbuf_height - 1;
    if (minx > maxx || miny > maxy) return;

    double area = edge_fn(v0.x,v0.y, v1.x,v1.y, v2.x,v2.y);
    if (abs(area) < 1e-8) return; /* degenerate */

    for (int y=miny;y<=maxy;y++){
        for (int x=minx;x<=maxx;x++){
            /* sample at pixel center */
            double px = (double)x + 0.5;
            double py = (double)y + 0.5;
            double w0 = edge_fn(v1.x,v1.y, v2.x,v2.y, px,py);
            double w1 = edge_fn(v2.x,v2.y, v0.x,v0.y, px,py);
            double w2 = edge_fn(v0.x,v0.y, v1.x,v1.y, px,py);
            /* barycentric inside test (same sign as area) */
            if ((w0>=0 && w1>=0 && w2>=0 && area>0) || (w0<=0 && w1<=0 && w2<=0 && area<0)){
                /* barycentric weights */
                double alpha = w0 / area;
                double beta  = w1 / area;
                double gamma = w2 / area;
                /* interpolate camera z (we use linear interpolation of camera-space z) */
                double depth = alpha * v0.cam_z + beta * v1.cam_z + gamma * v2.cam_z;
                /* depth test and paint */
                put_pixel_depth(x,y,depth,intensity,FB_BLUE);
            }
        }
    }
}

/* -------------------------------------------------------------------------- */
/* Higher-level renderer: transforms, culling, shading                        */
/* -------------------------------------------------------------------------- */

typedef struct {
    vec3 pos;   /* camera world position */
    double yaw; /* rotation around Y (radians) */
    double pitch; /* rotation around X (radians) */
} Camera;

static Camera camera = { .pos = {0.0, 0.0, 3.0}, .yaw = 0.0, .pitch = 0.0 };

/* directional light (world-space) */
static vec3 light_dir = { -0.5, -1.0, -0.5 };

/* single mesh for demo */
static Mesh cube;

/* render a mesh in world transform (model matrix) */
static void render_mesh(const Mesh *m, const mat4 model, const mat4 view, const mat4 proj){
    /* combined matrix M = proj * view * model */
    mat4 tmp, MVP;
    mat4_mul(tmp, view, model);      /* tmp = view * model */
    mat4_mul(MVP, proj, tmp);       /* MVP = proj * view * model */

    for (size i=0;i<m->tri_count;i++){
        Tri tri = m->tris[i];

        /* fetch model-space positions */
        vec3 a = m->verts[tri.a].pos;
        vec3 b = m->verts[tri.b].pos;
        vec3 c = m->verts[tri.c].pos;

        /* transform to world */
        vec4 wa = mat4_mul_vec4(model, v4(a.x,a.y,a.z,1.0));
        vec4 wb = mat4_mul_vec4(model, v4(b.x,b.y,b.z,1.0));
        vec4 wc = mat4_mul_vec4(model, v4(c.x,c.y,c.z,1.0));
        vec3 wpa = v3(wa.x, wa.y, wa.z);
        vec3 wpb = v3(wb.x, wb.y, wb.z);
        vec3 wpc = v3(wc.x, wc.y, wc.z);

        /* backface culling: compute normal in world space */
        vec3 face_normal = v3_norm(v3_cross(v3_sub(wpb, wpa), v3_sub(wpc, wpa)));

        /* view vector from triangle to camera */
        vec3 to_cam = v3_norm(v3_sub(camera.pos, wpa));
        double facing = v3_dot(face_normal, to_cam);
        if (facing <= 0.0) continue; /* backface */

        /* lighting */
        vec3 ld = v3_norm(light_dir);
        double intensity = v3_dot(face_normal, v3_mul(ld, -1.0));
        if (intensity < 0.0) intensity = 0.0;

        /* project vertices: transform with MVP */
        vec4 pa = mat4_mul_vec4(MVP, v4(a.x,a.y,a.z,1.0));
        vec4 pb = mat4_mul_vec4(MVP, v4(b.x,b.y,b.z,1.0));
        vec4 pc = mat4_mul_vec4(MVP, v4(c.x,c.y,c.z,1.0));

        /* perspective divide -> NDC */
        if (pa.w == 0.0 || pb.w == 0.0 || pc.w == 0.0) continue;
        vec4 nda = { pa.x/pa.w, pa.y/pa.w, pa.z/pa.w, 1.0/pa.w };
        vec4 ndb = { pb.x/pb.w, pb.y/pb.w, pb.z/pb.w, 1.0/pb.w };
        vec4 ndc = { pc.x/pc.w, pc.y/pc.w, pc.z/pc.w, 1.0/pc.w };

        /* trivial reject */
        if ((nda.x<-1 && ndb.x<-1 && ndc.x<-1) ||
            (nda.x> 1 && ndb.x> 1 && ndc.x> 1) ||
            (nda.y<-1 && ndb.y<-1 && ndc.y<-1) ||
            (nda.y> 1 && ndb.y> 1 && ndc.y> 1) ||
            (nda.z<-1 && ndb.z<-1 && ndc.z<-1) ||
            (nda.z> 1 && ndb.z> 1 && ndc.z> 1)) continue;

        /* camera-space Z for depth: transform with view only */
        vec4 ca = mat4_mul_vec4(view, v4(a.x,a.y,a.z,1.0));
        vec4 cb = mat4_mul_vec4(view, v4(b.x,b.y,b.z,1.0));
        vec4 cc = mat4_mul_vec4(view, v4(c.x,c.y,c.z,1.0));
        double camza = -ca.z;
        double camzb = -cb.z;
        double camzc = -cc.z;

        /* discard behind near plane */
        if (camza <= 0.0 || camzb <= 0.0 || camzc <= 0.0) continue;

        /* convert NDC -> screen */
        ScreenVert sv[3];
        int sx, sy;
        ndc_to_screen(nda.x, nda.y, &sx, &sy);
        sv[0].x = sx; sv[0].y = sy; sv[0].ndc_x = nda.x; sv[0].ndc_y = nda.y; sv[0].cam_z = camza;
        ndc_to_screen(ndb.x, ndb.y, &sx, &sy);
        sv[1].x = sx; sv[1].y = sy; sv[1].ndc_x = ndb.x; sv[1].ndc_y = ndb.y; sv[1].cam_z = camzb;
        ndc_to_screen(ndc.x, ndc.y, &sx, &sy);
        sv[2].x = sx; sv[2].y = sy; sv[2].ndc_x = ndc.x; sv[2].ndc_y = ndc.y; sv[2].cam_z = camzc;

        rasterize_triangle(sv[0], sv[1], sv[2], intensity);
    }
}

/* -------------------------------------------------------------------------- */
/* Demo state and main loop                                                    */
/* -------------------------------------------------------------------------- */

static void build_demo(void){
    cube = make_cube(0.5);
}

/* draw a tiny HUD */
static void draw_hud(void){
    int w = fb_width(), h = fb_height();
    int cx = w/2, cy = h/2;
    fb_drawline(cx-10, cy, cx+10, cy, 0xFFFFFF);
    fb_drawline(cx, cy-10, cx, cy+10, 0xFFFFFF);
}

/* global model rotation angle */
static double angle = 0.0;

void frame_step(void){
    int w = fb_width(), h = fb_height();
    ensure_zbuffer_size(w,h);
    fb_clear(0x000000);
    clear_zbuffer();

    double dt = 1.0 / (double)TARGET_FPS;
    double move_speed = 2.0 * dt;
    double rot_speed  = 1.2 * dt;

    if (get_keystate('w')) {
        camera.pos.x += -sin(camera.yaw) * move_speed;
        camera.pos.z += -cos(camera.yaw) * move_speed;
    }
    if (get_keystate('s')) {
        camera.pos.x -= -sin(camera.yaw) * move_speed;
        camera.pos.z -= -cos(camera.yaw) * move_speed;
    }
    if (get_keystate('a')) {
        camera.pos.x += -cos(camera.yaw) * move_speed;
        camera.pos.z +=  sin(camera.yaw) * move_speed;
    }
    if (get_keystate('d')) {
        camera.pos.x -= -cos(camera.yaw) * move_speed;
        camera.pos.z -=  sin(camera.yaw) * move_speed;
    }
    if (get_keystate('q')) camera.pos.y -= move_speed;
    if (get_keystate('e')) camera.pos.y += move_speed;

    /* arrow key codes — replace if your kernel uses different constants */
    if (get_keystate('j')) camera.yaw += rot_speed;
    if (get_keystate('l')) camera.yaw -= rot_speed;
    if (get_keystate('k')) camera.pitch += rot_speed;
    if (get_keystate('i')) camera.pitch -= rot_speed;

    /* model transform */
    mat4 model;
    mat4_rotation_y(model, angle);
    mat4 tmp;
    mat4_rotation_x(tmp, angle * 0.5);
    mat4_mul(model, model, tmp); /* model = Ry * Rx */

    /* view and projection */
    mat4 view;
    vec3 cam_forward = { -sin(camera.yaw), sin(camera.pitch), -cos(camera.yaw) };
    vec3 target = v3(camera.pos.x + cam_forward.x,
                     camera.pos.y + cam_forward.y,
                     camera.pos.z + cam_forward.z);
    mat4_look_at(view, camera.pos, target, v3(0,1,0));

    mat4 proj;
    double aspect = (double)w / (double)h;
    mat4_perspective(proj, CAMERA_FOV_DEG, aspect, CAMERA_NEAR, CAMERA_FAR);

    render_mesh(&cube, model, view, proj);
    draw_hud();

    angle += 0.8 * dt;
}

/* main loop called rmain */
void rmain(void){
    build_demo();
    while (1){
        frame_step();
        con_flush();
        sleep(1000 / TARGET_FPS);
    }
}

/* optional cleanup */
void engine_shutdown(void){
    free_mesh(&cube);
    free(zbuffer);
    zbuffer = NULL;
}
