#include <future>
#include <iostream>
#include <vector>

#include <tinyexr/tinyexr.h>

#include <imgui/imgui.h>
#include <imgui/backends/imgui_impl_opengl3.h>
#include <imgui/backends/imgui_impl_glfw.h>

#include <implot/implot.h>

#include "aperature.hpp"
#include "mesh.hpp"
#include "shader.hpp"
#include "logging.hpp"

constexpr int WINDOW_WIDTH = 1000;
constexpr int WINDOW_HEIGHT = 1000;

constexpr int RENDER_WIDTH = 1000;
constexpr int RENDER_HEIGHT = 1000;

GLFWwindow *glfw_init();

// Camera struct
static struct {
	glm::mat4 transform {1.0f};
	Aperature aperature {};
} camera;

// Framebuffer struct
struct Framebuffer {
	unsigned int framebuffer;

	// G-buffers
	unsigned int g_position;
	unsigned int g_normal;
	unsigned int g_material_index;
};

Framebuffer allocate_gl_framebuffer();

// All binding points
enum {
	PT_MATERIALS = GL_TEXTURE4,
};

// Path tracer information struct
struct {
	unsigned int materials_texture;
	unsigned int environment_map;
	unsigned int render_target;
} pt;

// Application state
struct {
	bool viewport_focused = false;
	bool viewport_hovered = false;
} app;

// Allocate the materials
struct CompressedMaterial {
	glm::vec4 diffuse;
	glm::vec4 specular;
	glm::vec4 emission;
	glm::vec4 roughness;
};

void allocate_pt_materials();
void imgui_init(GLFWwindow *);
void render_pt_pipeline(std::future <std::tuple <float *, int, int>> &, Framebuffer &, std::vector <GLBuffers> &, unsigned int, unsigned int);
void render_ui_pipeline();

void imgui_init(GLFWwindow *window)
{
	ImGui::CreateContext();
	ImPlot::CreateContext();
	ImGuiIO &io = ImGui::GetIO();

	io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
	io.ConfigWindowsMoveFromTitleBarOnly = true;

	// Fonts
	io.Fonts->AddFontFromFileTTF("../assets/fonts/Montserrat/static/Montserrat-SemiBold.ttf", 14);

	ImGui::StyleColorsDark();

	ImGui_ImplGlfw_InitForOpenGL(window, true);
	ImGui_ImplOpenGL3_Init("#version 450 core");
}

int main()
{
	// TODO: HDR output framebuffer?

	// Initialize GLFW
	GLFWwindow *window = glfw_init();
	if (!window)
		return -1;

	// Initialize ImGui
	imgui_init(window);

	// Load shaders
	unsigned int vertex_shader = compile_shader("../shaders/gbuffer.vert", GL_VERTEX_SHADER);
	unsigned int fragment_shader = compile_shader("../shaders/gbuffer.frag", GL_FRAGMENT_SHADER);
	unsigned int path_tracer_shader = compile_shader("../shaders/render.glsl", GL_COMPUTE_SHADER);

	// Create shader programs
	unsigned int shader_program = glCreateProgram();
	glAttachShader(shader_program, vertex_shader);
	glAttachShader(shader_program, fragment_shader);
	link_program(shader_program);

	unsigned int path_tracer_program = glCreateProgram();
	glAttachShader(path_tracer_program, path_tracer_shader);
	link_program(path_tracer_program);

	// Load model and all its buffers
	Model model = load_model("../../models/cornell_box/CornellBox-Original.obj");

	std::vector <GLBuffers> buffers;
	for (const Mesh &mesh : model.meshes)
		buffers.push_back(allocate_gl_buffers(&mesh));

	printf("# of emissive meshes: %lu\n", model.emissive_meshes.size());

	// Enable depth testing
	glEnable(GL_DEPTH_TEST);

	// Create the framebuffer
	Framebuffer fb = allocate_gl_framebuffer();

	// Allocate a destination texture for the compute shader
	glGenTextures(1, &pt.render_target);
	glBindTexture(GL_TEXTURE_2D, pt.render_target);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, RENDER_WIDTH, RENDER_HEIGHT, 0, GL_RGBA, GL_FLOAT, NULL);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

	// Unbind framebuffer
	glBindFramebuffer(GL_FRAMEBUFFER, 0);

	// Allocate PT resources
	allocate_pt_materials();

	// Test loading EXR
	auto exr_loader = [&]() -> std::tuple <float *, int, int> {
		float *data;
		int width;
		int height;

		const char *filename = "../../downloads/095_hdrmaps_com_free.exr";
		const char *error;

		int ret = LoadEXR(&data, &width, &height, filename, &error);
		if (ret != 0) {
			logf(eLogError, "Error loading EXR: %s", error);
			FreeEXRErrorMessage(error);
			return {nullptr, 0, 0};
		}

		logf(eLogInfo, "Loaded EXR: %d x %d", width, height);
		return {data, width, height};
	};

	auto future = std::async(std::launch::async, exr_loader);

	// Main loop
	while (!glfwWindowShouldClose(window)) {
		constexpr float speed = 0.25f;

		// Handle camera movement
		if (app.viewport_focused) {
			glm::vec3 diff {0.0};
			if (glfwGetKey(window, GLFW_KEY_W) == GLFW_PRESS)
				diff.z -= speed;
			if (glfwGetKey(window, GLFW_KEY_A) == GLFW_PRESS)
				diff.x -= speed;
			if (glfwGetKey(window, GLFW_KEY_E) == GLFW_PRESS)
				diff.y += speed;

			if (glfwGetKey(window, GLFW_KEY_D) == GLFW_PRESS)
				diff.x += speed;
			if (glfwGetKey(window, GLFW_KEY_S) == GLFW_PRESS)
				diff.z += speed;
			if (glfwGetKey(window, GLFW_KEY_Q) == GLFW_PRESS)
				diff.y -= speed;

			camera.transform = glm::translate(camera.transform, diff);
		}

		// Render the scene
		render_pt_pipeline(future, fb, buffers, shader_program, path_tracer_program);

		// Render the UI
		render_ui_pipeline();

		// Swap buffers
		glfwSwapBuffers(window);
		glfwPollEvents();
	}

	// Clean up
	glfwTerminate();

	// TODO: clean up buffers and everything else

	return 0;
}

static bool dragging = false;

static void mouse_callback(GLFWwindow *window, double xpos, double ypos)
{
	static double last_x = WINDOW_WIDTH/2.0;
	static double last_y = WINDOW_HEIGHT/2.0;

	static float sensitivity = 0.1f;

	static bool first_mouse = true;

	static float yaw = 0.0f;
	static float pitch = 0.0f;

	if (first_mouse) {
		last_x = xpos;
		last_y = ypos;
		first_mouse = false;
	}

	double xoffset = last_x - xpos;
	double yoffset = last_y - ypos;

	last_x = xpos;
	last_y = ypos;

	xoffset *= sensitivity;
	yoffset *= sensitivity;

	// Only drag when left mouse button is pressed
	if (dragging) {
		yaw += xoffset;
		pitch += yoffset;

		// Clamp pitch
		if (pitch > 89.0f)
			pitch = 89.0f;
		if (pitch < -89.0f)
			pitch = -89.0f;

		// Set camera transform yaw and pitch
		glm::mat4 &transform = camera.transform;

		// First decompose the transform matrix
		glm::vec3 translation;
		glm::vec3 scale;
		glm::quat rotation;
		glm::vec3 skew;
		glm::vec4 perspective;

		glm::decompose(transform, scale, rotation, translation, skew, perspective);

		// Then set the rotation
		rotation = glm::quat(glm::vec3(glm::radians(pitch), glm::radians(yaw), 0.0f));

		// Finally recompose the transform matrix
		transform = glm::translate(glm::mat4 {1.0f}, translation);
		transform = transform * glm::mat4_cast(rotation);
		transform = glm::scale(transform, scale);
	}
}

void mouse_button_callback(GLFWwindow *window, int button, int action, int mods)
{
	if (button == GLFW_MOUSE_BUTTON_LEFT
			&& action == GLFW_PRESS
			&& app.viewport_hovered) {
		glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
		dragging = true;
	} else if ((button == GLFW_MOUSE_BUTTON_LEFT && action == GLFW_RELEASE)
			|| !app.viewport_hovered) {
		glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
		dragging = false;
	}
}

GLFWwindow *glfw_init()
{
	// Basic window
	GLFWwindow *window = nullptr;
	if (!glfwInit())
		return nullptr;

	window = glfwCreateWindow(WINDOW_WIDTH, WINDOW_HEIGHT, "SDF Engine", NULL, NULL);

	// Check if window was created
	if (!window) {
		glfwTerminate();
		return nullptr;
	}

	// Make the window's context current
	glfwMakeContextCurrent(window);

	// Load OpenGL functions using GLAD
	if (!gladLoadGLLoader((GLADloadproc) glfwGetProcAddress)) {
		fprintf(stderr, "Failed to initialize OpenGL context\n");
		return nullptr;
	}

	// Set up callbacks
	glfwSetCursorPosCallback(window, mouse_callback);
	glfwSetMouseButtonCallback(window, mouse_button_callback);
	// glfwSetKeyCallback(window, keyboard_callback);

	const GLubyte* renderer = glGetString(GL_RENDERER);
	printf("Renderer: %s\n", renderer);

	return window;
}

Framebuffer allocate_gl_framebuffer()
{
	// Generate G-buffer for custom path tracer
	unsigned int g_buffer;
	glGenFramebuffers(1, &g_buffer);

	// Generate textures for G-buffer
	unsigned int g_position;
	glGenTextures(1, &g_position);
	glBindTexture(GL_TEXTURE_2D, g_position);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB16F, RENDER_WIDTH, RENDER_HEIGHT, 0, GL_RGB, GL_FLOAT, NULL);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

	unsigned int g_normal;
	glGenTextures(1, &g_normal);
	glBindTexture(GL_TEXTURE_2D, g_normal);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB16F, RENDER_WIDTH, RENDER_HEIGHT, 0, GL_RGB, GL_FLOAT, NULL);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

	unsigned int g_material_index;
	glGenTextures(1, &g_material_index);
	glBindTexture(GL_TEXTURE_2D, g_material_index);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_R32UI, RENDER_WIDTH, RENDER_HEIGHT, 0, GL_RED_INTEGER, GL_UNSIGNED_INT, NULL);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

	// Attach textures to G-buffer
	glBindFramebuffer(GL_FRAMEBUFFER, g_buffer);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_position, 0);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, g_normal, 0);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT2, GL_TEXTURE_2D, g_material_index, 0);

	// Set draw buffers
	unsigned int attachments[3] = {
		GL_COLOR_ATTACHMENT0,
		GL_COLOR_ATTACHMENT1,
		GL_COLOR_ATTACHMENT2
	};

	glDrawBuffers(3, attachments);

	// Depth buffer
	unsigned int g_depth;
	glGenTextures(1, &g_depth);
	glBindTexture(GL_TEXTURE_2D, g_depth);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT, RENDER_WIDTH, RENDER_HEIGHT, 0, GL_DEPTH_COMPONENT, GL_FLOAT, NULL);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, g_depth, 0);

	// Check if framebuffer is complete
	if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
		fprintf(stderr, "Framebuffer is not complete!\n");
		return {};
	}

	glBindFramebuffer(GL_FRAMEBUFFER, 0);

	return Framebuffer {
		g_buffer,
		g_position,
		g_normal,
		g_material_index,
	};
}

void allocate_pt_materials()
{
	constexpr unsigned int stride = sizeof(CompressedMaterial)/sizeof(glm::vec4);

	// TODO: Colored logging
	printf("Allocating %lu materials for path tracer\n", Material::all.size());
	std::vector <CompressedMaterial> materials;
	for (const Material &material : Material::all) {
		CompressedMaterial compressed_material;

		compressed_material.diffuse = glm::vec4 {material.diffuse, 1.0f};
		compressed_material.specular = glm::vec4 {material.specular, 1.0f};
		compressed_material.emission = glm::vec4 {material.emission, 1.0f};
		compressed_material.roughness = glm::vec4 {material.roughness};

		materials.push_back(compressed_material);
	}

	glGenTextures(1, &pt.materials_texture);
	glBindTexture(GL_TEXTURE_2D, pt.materials_texture);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, stride * materials.size(), 1, 0, GL_RGBA, GL_FLOAT, materials.data());
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

	glBindTexture(GL_TEXTURE_2D, 0);
}

void render_pt_pipeline(std::future <std::tuple <float *, int, int>> &future, Framebuffer &fb, std::vector <GLBuffers> &buffers, unsigned int shader_program, unsigned int path_tracer_program)
{
	// Bind framebuffer
	glBindFramebuffer(GL_FRAMEBUFFER, fb.framebuffer);

	// Clear
	glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

	// Load shader and set uniforms
	glUseProgram(shader_program);

	glm::mat4 view = camera.aperature.view_matrix(camera.transform);
	// TODO: pass extent to this method
	glm::mat4 projection = camera.aperature.perspective_matrix();

	glm::mat4 model {1.0f};
	glm::scale(model, glm::vec3 {1.0f});

	set_mat4(shader_program, "model", model);
	set_mat4(shader_program, "view", view);
	set_mat4(shader_program, "projection", projection);

	// TODO: use common VAO...
	glBindVertexArray(buffers[0].vao);
	for (const GLBuffers &buffer : buffers) {
		unsigned int material_index = buffer.source->material_index;
		set_uint(shader_program, "material_index", material_index);

		glBindVertexArray(buffer.vao);
		glDrawElements(GL_TRIANGLES, buffer.count, GL_UNSIGNED_INT, 0);
	}

	// Run the compute shader
	glUseProgram(path_tracer_program);

	// Bind all the textures
	glActiveTexture(GL_TEXTURE1);
	glBindTexture(GL_TEXTURE_2D, fb.g_position);

	glActiveTexture(GL_TEXTURE2);
	glBindTexture(GL_TEXTURE_2D, fb.g_normal);

	glActiveTexture(GL_TEXTURE4);
	glBindTexture(GL_TEXTURE_2D, fb.g_material_index);

	glActiveTexture(GL_TEXTURE3);
	glBindTexture(GL_TEXTURE_2D, pt.materials_texture);

	// Environment map loading
	if (future.valid()) {
		if (future.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
			std::tuple <float *, int, int> result = future.get();

			float *data = std::get <0> (result);
			int width = std::get <1> (result);
			int height = std::get <2> (result);

			// Create environment map texture
			printf("Creating environment map texture\n");
			glGenTextures(1, &pt.environment_map);
			glBindTexture(GL_TEXTURE_2D, pt.environment_map);
			glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, width, height, 0, GL_RGBA, GL_FLOAT, data);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
			free(data);

			glBindTexture(GL_TEXTURE_2D, 0);

			// TODO: Trigger a popup (or go to the log...)
		}

		glActiveTexture(GL_TEXTURE5);
		glBindTexture(GL_TEXTURE_2D, 0);
	} else {
		glActiveTexture(GL_TEXTURE5);
		glBindTexture(GL_TEXTURE_2D, pt.environment_map);
	}

	glBindImageTexture(0, pt.render_target, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA32F);

	// Set the uniforms
	auto uvw = uvw_frame(camera.aperature, camera.transform);
	set_vec3(path_tracer_program, "camera.position", camera.transform[3]);
	set_vec3(path_tracer_program, "camera.axis_u", std::get <0> (uvw));
	set_vec3(path_tracer_program, "camera.axis_v", std::get <1> (uvw));
	set_vec3(path_tracer_program, "camera.axis_w", std::get <2> (uvw));

	// Run the shader
	glDispatchCompute(RENDER_WIDTH, RENDER_HEIGHT, 1);
}

void render_ui_pipeline()
{
	static auto start_time = std::chrono::high_resolution_clock::now();

	// auto start_time = std::chrono::high_resolution_clock::now();
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

	// Start the Dear ImGui frame
	ImGui_ImplOpenGL3_NewFrame();
	ImGui_ImplGlfw_NewFrame();

	ImGui::NewFrame();

	// Docking space
	ImGui::DockSpaceOverViewport(ImGui::GetMainViewport());

	// UI elements
	ImGui::Begin("Performance");
		ImGui::Text("FPS: %.1f", ImGui::GetIO().Framerate);

		// Plot the frame times over 5 seconds
		using frame_time = std::pair <float, float>;
		static std::vector <frame_time> frames;

		float fps = ImGui::GetIO().Framerate;
		float time = std::chrono::duration <float> (std::chrono::high_resolution_clock::now() - start_time).count();
		frames.push_back({time, fps});

		// Remove old frame times
		while (frames.size() > 0 && frames.front().first < time - 5.0f)
			frames.erase(frames.begin());

		// Plot the frame times
		ImPlot::SetNextAxesLimits(0, 5, 0, 165, ImGuiCond_Always);
		if (ImPlot::BeginPlot("Frame times")) {
			std::vector <float> times;
			std::vector <float> fpses;

			float min_time = frames.front().first;
			for (auto &frame : frames) {
				times.push_back(frame.first - min_time);
				fpses.push_back(frame.second);
			}

			// Set limits
			ImPlot::PlotLine("FPS", times.data(), fpses.data(), times.size());


			ImPlot::EndPlot();
		}
	ImGui::End();

	ImGui::Begin("Viewport");
		constexpr float padding = 10;

		// Get ImGui window size
		ImVec2 window_size = ImGui::GetWindowSize();
		window_size.x -= padding * 2;
		window_size.y -= padding * 2;

		// Set the camera aspect ratio for the next frame
		camera.aperature.m_aspect = window_size.x / window_size.y;

		// Render the framebuffer
		ImGui::Image(
			(void *)(intptr_t) pt.render_target,
			window_size,
			ImVec2 {0, 1}, ImVec2 {1, 0}
		);

		// Check if the window has focus
		app.viewport_hovered = ImGui::IsItemHovered();
		app.viewport_focused = ImGui::IsWindowFocused();
	ImGui::End();

	ImGui::EndFrame();

	// Update platform windows
	ImGui::UpdatePlatformWindows();

	// Render UI
	ImGui::Render();
	ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
}
