#include <GL/glew.h>

#include "radeon_rays.h"
#include "radeon_rays_cl.h"
#include "CLW.h"

#include <GL/glut.h>
#include <cassert>
#include <iostream>
#include <memory>
#include <random>
#include "../Tools/shader_manager.h"
#include "../Tools/tiny_obj_loader.h"
#include "hdr.h"

using namespace RadeonRays;
using namespace tinyobj;

namespace {
    std::vector<shape_t> g_objshapes;
    std::vector<material_t> g_objmaterials;
    GLuint g_vertex_buffer, g_index_buffer;
    GLuint g_texture;
    int g_window_width = 1024;
    int g_window_height = 1024;
    std::unique_ptr<ShaderManager> g_shader_manager;
    
    IntersectionApi* g_api;

    //CL data
    CLWContext g_context;
    CLWProgram g_program;
    CLWBuffer<float> g_positions;
    CLWBuffer<float> g_normals;
    CLWBuffer<int> g_indices;
    CLWBuffer<float> g_colors;
    CLWBuffer<int> g_indent;


    struct Camera
    {
        // Camera coordinate frame
        float3 forward;
        float3 up;
        float3 p;

        // Near and far Z
        float2 zcap;
    };
}

cl_device_id id;
cl_command_queue queue;

void* MapCLWBuffer(ReferenceCounter<cl_mem, clRetainMemObject, clReleaseMemObject> buf, int sz) {
	return clEnqueueMapBuffer(queue, (cl_mem)buf, true, CL_MAP_READ, 0, sz, 0, NULL, NULL, NULL);
}

void UnmapCLWBuffer(ReferenceCounter<cl_mem, clRetainMemObject, clReleaseMemObject> buf, void* ptr) {
	clEnqueueUnmapMemObject(queue, (cl_mem)buf, ptr, 0, NULL, NULL);
}

CLWBuffer<float> bg_buf;

void InitData()
{
    //Load
    std::string basepath = "../../Resources/CornellBox/"; 
    std::string filename = basepath + "sharo.obj";
    std::string res = LoadObj(g_objshapes, g_objmaterials, filename.c_str(), basepath.c_str());
    if (res != "")
    {
        throw std::runtime_error(res);
    }

    // Load data to CL
    std::vector<float> verts;
    std::vector<float> normals;
    std::vector<int> inds;
    std::vector<float> colors;
    std::vector<int> indents;
    int indent = 0;

    for (int id = 0; id < g_objshapes.size(); ++id)
    {
        const mesh_t& mesh = g_objshapes[id].mesh;
        verts.insert(verts.end(), mesh.positions.begin(), mesh.positions.end());
        normals.insert(normals.end(), mesh.normals.begin(), mesh.normals.end());
        inds.insert(inds.end(), mesh.indices.begin(), mesh.indices.end());
        for (int mat_id : mesh.material_ids)
        {
            const material_t& mat = g_objmaterials[mat_id];
            colors.push_back(mat.diffuse[0]);
            colors.push_back(mat.diffuse[1]);
            colors.push_back(mat.diffuse[2]);
        }
        
        // add additional emty data to simplify indentation in arrays
        //if (mesh.positions.size() / 3 < mesh.indices.size())
        //{
        //    int count = mesh.indices.size() - mesh.positions.size() / 3;
        //    for (int i = 0; i < count; ++i)
        //    {
        //        verts.push_back(0.f); normals.push_back(0.f);
        //        verts.push_back(0.f); normals.push_back(0.f);
        //        verts.push_back(0.f); normals.push_back(0.f);
        //    }
        //}
		verts.resize(mesh.indices.size() * 3, 0.0f);
		normals.resize(mesh.indices.size() * 3, 0.0f);

        indents.push_back(indent);
        indent += mesh.indices.size();
    }
    g_positions = CLWBuffer<float>::Create(g_context, CL_MEM_READ_ONLY, verts.size(), verts.data());
    g_normals = CLWBuffer<float>::Create(g_context, CL_MEM_READ_ONLY, normals.size(), normals.data());
    g_indices = CLWBuffer<int>::Create(g_context, CL_MEM_READ_ONLY, inds.size(), inds.data());
    g_colors = CLWBuffer<float>::Create(g_context, CL_MEM_READ_ONLY, colors.size(), colors.data());
    g_indent = CLWBuffer<int>::Create(g_context, CL_MEM_READ_ONLY, indents.size(), indents.data());

	unsigned int _w, _h;
	auto d = hdr::read_hdr("refl.hdr", &_w, &_h);
	float* dv = new float[_w*_h * 3];
	hdr::to_float(d, _w, _h, dv);
	bg_buf = CLWBuffer<float>::Create(g_context, CL_MEM_READ_ONLY, 3 * _w * _h, dv);
}

CLWBuffer<ray> GeneratePrimaryRays()
{
    //prepare camera buf
    Camera cam;
    cam.forward = {0.f, 0.f, 1.f };
    cam.up = { 0.f, 1.f, 0.f };
    cam.p = { 0.f, 1.f, 5.f };
    cam.zcap = { 1.f, 1000.f };
    CLWBuffer<Camera> camera_buf = CLWBuffer<Camera>::Create(g_context, CL_MEM_READ_ONLY, 1, &cam);

    //run kernel
    CLWBuffer<ray> ray_buf = CLWBuffer<ray>::Create(g_context, CL_MEM_READ_WRITE, g_window_width*g_window_height);
    CLWKernel kernel = g_program.GetKernel("GeneratePerspectiveRays");
    kernel.SetArg(0, ray_buf);
    kernel.SetArg(1, camera_buf);
    kernel.SetArg(2, g_window_width);
    kernel.SetArg(3, g_window_height);

    // Run generation kernel
    size_t gs[] = { static_cast<size_t>((g_window_width + 7) / 8 * 8), static_cast<size_t>((g_window_height + 7) / 8 * 8) };
    size_t ls[] = { 8, 8 };
    g_context.Launch2D(0, gs, ls, kernel);
    g_context.Flush(0);

    return ray_buf;
}

CLWBuffer<float> accum;
int samples = 0;

void Shading(CLWBuffer<unsigned char> out_buff, const CLWBuffer<Intersection> &isect, CLWBuffer<float>& col_buff, CLWBuffer<ray>& ray_buff, const int smps)
{
    //run kernel
    CLWKernel kernel = g_program.GetKernel("Shading");
    kernel.SetArg(0, g_positions);
    kernel.SetArg(1, g_normals);
    kernel.SetArg(2, g_indices);
    kernel.SetArg(3, g_colors);
    kernel.SetArg(4, g_indent);
    kernel.SetArg(5, isect);
    kernel.SetArg(6, 1);
    kernel.SetArg(7, g_window_width);
    kernel.SetArg(8, g_window_height);
    kernel.SetArg(9, out_buff);
    kernel.SetArg(10, col_buff);
    kernel.SetArg(11, ray_buff);
    kernel.SetArg(12, cl_int(rand() % RAND_MAX));
    kernel.SetArg(13, accum);
    kernel.SetArg(14, smps);
	kernel.SetArg(15, bg_buf);

    // Run generation kernel
    size_t gs[] = { static_cast<size_t>((g_window_width + 7) / 8 * 8), static_cast<size_t>((g_window_height + 7) / 8 * 8) };
    size_t ls[] = { 8, 8 };
    g_context.Launch2D(0, gs, ls, kernel);
    g_context.Flush(0);
}

void Trace();

void DrawScene()
{

    glDisable(GL_DEPTH_TEST);
    glViewport(0, 0, g_window_width, g_window_height);

    glClear(GL_COLOR_BUFFER_BIT);

    glBindBuffer(GL_ARRAY_BUFFER, g_vertex_buffer);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g_index_buffer);

    // shader data
    GLuint program = g_shader_manager->GetProgram("simple");
    glUseProgram(program);
    GLuint texloc = glGetUniformLocation(program, "g_Texture");
    assert(texloc >= 0);

    glUniform1i(texloc, 0);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, g_texture);

    GLuint position_attr = glGetAttribLocation(program, "inPosition");
    GLuint texcoord_attr = glGetAttribLocation(program, "inTexcoord");
    glVertexAttribPointer(position_attr, 3, GL_FLOAT, GL_FALSE, sizeof(float) * 5, 0);
    glVertexAttribPointer(texcoord_attr, 2, GL_FLOAT, GL_FALSE, sizeof(float) * 5, (void*)(sizeof(float) * 3));
    glEnableVertexAttribArray(position_attr);
    glEnableVertexAttribArray(texcoord_attr);

    // draw rectangle
    glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, nullptr);

    glDisableVertexAttribArray(texcoord_attr);
    glBindTexture(GL_TEXTURE_2D, 0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    glUseProgram(0);

    glFinish();
    glutSwapBuffers();
    glutPostRedisplay();

    Trace();
}

void InitGl()
{
    g_shader_manager.reset(new ShaderManager());

    glClearColor(0.0, 0.0, 0.0, 0.0);
    glCullFace(GL_NONE);
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_TEXTURE_2D);

    glGenBuffers(1, &g_vertex_buffer);
    glGenBuffers(1, &g_index_buffer);

    // create Vertex buffer
    glBindBuffer(GL_ARRAY_BUFFER, g_vertex_buffer);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g_index_buffer);

    float quad_vdata[] =
    {
        -1, -1, 0.5, 0, 0,
        1, -1, 0.5, 1, 0,
        1, 1, 0.5, 1, 1,
        -1, 1, 0.5, 0, 1
    };

    GLshort quad_idata[] =
    {
        0, 1, 3,
        3, 1, 2
    };

    // fill data
    glBufferData(GL_ARRAY_BUFFER, sizeof(quad_vdata), quad_vdata, GL_STATIC_DRAW);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(quad_idata), quad_idata, GL_STATIC_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);

    // texture
    glGenTextures(1, &g_texture);
    glBindTexture(GL_TEXTURE_2D, g_texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, g_window_width, g_window_height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glBindTexture(GL_TEXTURE_2D, 0);
}

void InitCl()
{
    std::vector<CLWPlatform> platforms;
    CLWPlatform::CreateAllPlatforms(platforms);

    if (platforms.size() == 0)
    {
        throw std::runtime_error("No OpenCL platforms installed.");
    }

    for (int i = 0; i < platforms.size(); ++i)
    {
        for (int d = 0; d < (int)platforms[i].GetDeviceCount(); ++d)
        {
            if (platforms[i].GetDevice(d).GetType() != CL_DEVICE_TYPE_GPU)
                continue;
            g_context = CLWContext::Create(platforms[i].GetDevice(d));
            break;
        }

        if (g_context)
            break;
    }
    const char* kBuildopts(" -cl-mad-enable -cl-fast-relaxed-math -cl-std=CL1.2 -I . ");

    g_program = CLWProgram::CreateFromFile("kernel.cl", kBuildopts, g_context);

    float* zeros = new float[3*g_window_width*g_window_height]{};
    accum = CLWBuffer<float>::Create(g_context, CL_MEM_READ_WRITE, 3*g_window_width*g_window_height, zeros);
    delete[](zeros);
}

void Trace() {
    const int k_raypack_size = g_window_height * g_window_width;
    // Prepare rays. One for each texture pixel.
    CLWBuffer<ray> ray_buffer_cl = GeneratePrimaryRays();
	Buffer* ray_buffer = CreateFromOpenClBuffer(g_api, ray_buffer_cl);
    // Intersection data
    CLWBuffer<Intersection> isect_buffer_cl = CLWBuffer<Intersection>::Create(g_context, CL_MEM_READ_WRITE, g_window_width*g_window_height);
    Buffer* isect_buffer = CreateFromOpenClBuffer(g_api, isect_buffer_cl);
    
    // Intersection
    g_api->QueryIntersection(ray_buffer, k_raypack_size, isect_buffer, nullptr, nullptr);

    // Point light position
    float3 light = { -0.01f, 1.85f, 0.1f };

    float* zeros = new float[4*g_window_width*g_window_height]{};
    CLWBuffer<unsigned char> out_buff = CLWBuffer<unsigned char>::Create(g_context, CL_MEM_WRITE_ONLY, 4*g_window_width*g_window_height, zeros);
    
    CLWBuffer<float> col_buff = CLWBuffer<float>::Create(g_context, CL_MEM_READ_WRITE, 4*g_window_width*g_window_height, zeros);
    
    delete[](zeros);

    // Shading
	Event* e = nullptr;

	//*
    for (int a = 0; a < 2; a++) {
        Shading(out_buff, isect_buffer_cl, col_buff, ray_buffer_cl, 1+samples);
		delete(ray_buffer);
		ray_buffer = CreateFromOpenClBuffer(g_api, ray_buffer_cl);
        g_api->QueryIntersection(ray_buffer, k_raypack_size, isect_buffer, nullptr, &e);
		e->Wait();
    }
    Shading(out_buff, isect_buffer_cl, col_buff, ray_buffer_cl, ++samples);
	//*/

	//Shading(out_buff, isect_buffer_cl, light, col_buff, ray_buffer_cl, 1, ++samples);
	delete(ray_buffer);

    //Buffer* tex_buf = CreateFromOpenClBuffer(g_api, out_buff);
    
    // Get image data
    //g_api->MapBuffer(tex_buf, kMapRead, 0, 4 * k_raypack_size * sizeof(unsigned char), (void**)&pixels, &e);
    //e->Wait();
	void* pixels = MapCLWBuffer(out_buff, 4 * k_raypack_size);

    // Update texture data
    glBindTexture(GL_TEXTURE_2D, g_texture);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, g_window_width, g_window_height, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    glBindTexture(GL_TEXTURE_2D, NULL);

	//g_api->UnmapBuffer(tex_buf, pixels, nullptr);
	UnmapCLWBuffer(out_buff, pixels);

	delete(isect_buffer);
}

int main(int argc, char* argv[])
{
    // GLUT Window Initialization:
    glutInit(&argc, (char**)argv);
    glutInitWindowSize(g_window_width, g_window_height);
    glutInitDisplayMode(GLUT_RGBA | GLUT_DOUBLE | GLUT_DEPTH);
    glutCreateWindow("TutorialCornellBoxShadow");

    GLenum err = glewInit();
    if (err != GLEW_OK)
    {
        std::cout << "GLEW initialization failed\n";
        return -1;
    }
    // Prepare rectangle for drawing texture
    // rendered using intersection results
    InitGl();

    InitCl();

    // Load CornellBox model
    InitData();

    // Create api using already exist opencl context
    id = g_context.GetDevice(0).GetID();
    queue = g_context.GetCommandQueue(0);

    // Create intersection API
	g_api = RadeonRays::CreateFromOpenClContext(g_context, id, queue);

    // Adding meshes to tracing scene
    for (int id = 0; id < g_objshapes.size(); ++id)
    {
        shape_t& objshape = g_objshapes[id];
        float* vertdata = objshape.mesh.positions.data();
        int nvert = objshape.mesh.positions.size() / 3;
        int* indices = objshape.mesh.indices.data();
        int nfaces = objshape.mesh.indices.size() / 3;
        Shape* shape = g_api->CreateMesh(vertdata, nvert, 3 * sizeof(float), indices, 0, nullptr, nfaces);

        assert(shape != nullptr);
        g_api->AttachShape(shape);
        shape->SetId(id);
    }
    // Commit scene changes
    g_api->Commit();

    Trace();

    // Start the main loop
    glutDisplayFunc(DrawScene);
    glutMainLoop();

    // Cleanup
    IntersectionApi::Delete(g_api); g_api = nullptr;

    return 0;
}
