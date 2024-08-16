#include <torch/script.h>

#include <iostream>
#include <memory>
#include <filesystem>
#include <string>

#include <raylib.h>
#include <raymath.h>

#if defined(_WIN32)           
#define NOGDI             // All GDI defines and routines
#define NOUSER            // All USER defines and routines
#endif

#include <Windows.h> // or any library that uses Windows.h

#if defined(_WIN32)           // raylib uses these names as function parameters
#undef near
#undef far
#endif

#pragma comment(lib, "winmm.lib")

#pragma region Torch

const int sampleRate = 16000;        // 16 kHz sample rate
const int bitsPerSample = 8;        // 8-bit sample size
const int numChannels = 1;           // Mono channel
const int byteRate = 256000;         // 256 kbps bitrate
const int blockAlign = numChannels * bitsPerSample / 8;
const int bufferSize = sampleRate * numChannels * (bitsPerSample / 8);  // 1 second buffer

std::vector<short> buffer(bufferSize);

std::string classnames[] = { "up", "down", "left", "right" };

HWAVEIN hWaveIn;
WAVEFORMATEX wfx = { 0 };
WAVEHDR waveHdr = { 0 };

torch::jit::script::Module module;

int torchInit() {
	std::cout << std::filesystem::current_path() << "\n";
	try
	{
		module = torch::jit::load(std::filesystem::current_path().concat("\\model_scripted_cpu.pt").string(), torch::kCPU);
	}
	catch (const std::exception& e)
	{
		std::cout << e.what();

		return -1;
	}
}

std::string torchInference() {
	try
	{
		std::vector<torch::jit::IValue> inputs;

		auto tensor = torch::tensor(buffer, torch::TensorOptions().dtype(torch::kFloat32));

		auto stftd = torch::stft(
			tensor,
			256,
			floor(256 / 8),
			256,
			torch::hann_window(256),
			true,
			"reflect",
			false,
			true,
			true).abs();

		torch::Tensor audio = torch::pow(
			stftd
			, 2).unsqueeze_(0);

		audio = torch::divide(audio, 256);

		inputs.push_back(audio);

		module.eval();
		torch::Tensor pred = torch::softmax(module.forward(inputs).toTensor(), 1).argmax(1);

		torch::Tensor pred_int = pred.to(c10::ScalarType::Int);

		int* temp_arr = pred_int.data_ptr<int>();

		int arr = temp_arr[0];

		return classnames[arr];
	}
	catch (const std::exception& e)
	{
		std::cout << e.what();
	}
	return "Nan";
}

void writeWavFile(const char* filename, short* data, int dataSize) {
	std::ofstream file(filename, std::ios::binary);

	// Write the WAV file header
	file.write("RIFF", 4);
	int chunkSize = 36 + dataSize;
	file.write(reinterpret_cast<const char*>(&chunkSize), 4);
	file.write("WAVE", 4);

	// Format chunk
	file.write("fmt ", 4);
	int subChunk1Size = 16;
	short audioFormat = 1;
	short numChannels = 1;
	short bitsPerSample = 16;
	short blockAlign = numChannels * bitsPerSample / 8;
	int byteRate = sampleRate * blockAlign;

	file.write(reinterpret_cast<const char*>(&subChunk1Size), 4);
	file.write(reinterpret_cast<const char*>(&audioFormat), 2);
	file.write(reinterpret_cast<const char*>(&numChannels), 2);
	file.write(reinterpret_cast<const char*>(&sampleRate), 4);
	file.write(reinterpret_cast<const char*>(&byteRate), 4);
	file.write(reinterpret_cast<const char*>(&blockAlign), 2);
	file.write(reinterpret_cast<const char*>(&bitsPerSample), 2);

	// Data chunk
	file.write("data", 4);
	file.write(reinterpret_cast<const char*>(&dataSize), 4);
	file.write(reinterpret_cast<const char*>(data), dataSize);

	file.close();
}

void startRecording() {
	// Set up WAVEFORMATEX
	wfx.nSamplesPerSec = sampleRate;       // 16 kHz sample rate
	wfx.wBitsPerSample = bitsPerSample;    // 16-bit sample size
	wfx.nChannels = numChannels;           // Mono
	wfx.cbSize = 0;
	wfx.wFormatTag = WAVE_FORMAT_PCM;
	wfx.nBlockAlign = wfx.nChannels * wfx.wBitsPerSample / 8;
	wfx.nAvgBytesPerSec = byteRate;        // 256 kbps

	// Open the default waveform audio input device
	if (waveInOpen(&hWaveIn, WAVE_MAPPER, &wfx, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) {
		std::cerr << "Failed to open waveform input device." << std::endl;
		return;
	}

	// Prepare the WAVEHDR structure
	waveHdr.lpData = reinterpret_cast<LPSTR>(buffer.data());
	waveHdr.dwBufferLength = bufferSize;
	waveHdr.dwFlags = 0;

	if (waveInPrepareHeader(hWaveIn, &waveHdr, sizeof(WAVEHDR)) != MMSYSERR_NOERROR) {
		std::cerr << "Failed to prepare header." << std::endl;
		return;
	}

	// Add buffer to the queue
	if (waveInAddBuffer(hWaveIn, &waveHdr, sizeof(WAVEHDR)) != MMSYSERR_NOERROR) {
		std::cerr << "Failed to add buffer." << std::endl;
		return;
	}

	// Start recording
	if (waveInStart(hWaveIn) != MMSYSERR_NOERROR) {
		std::cerr << "Failed to start recording." << std::endl;
		return;
	}

	std::cout << "Recording for 1 second..." << std::endl;
	Sleep(1000);  // Record for 0.1 second
}

void stopRecording() {
	// Stop recording
	waveInStop(hWaveIn);
	waveInUnprepareHeader(hWaveIn, &waveHdr, sizeof(WAVEHDR));
	waveInClose(hWaveIn);

	std::cout << "Recording stopped." << std::endl;
}

#pragma endregion

#pragma region Game

const int num_cells = 40;
const int cell_size = 20;
const int screen_width = num_cells * cell_size;
const int screen_height = num_cells * cell_size;

class Food {
public:
	Vector2 pos;
	Food() { GenerateNewPosition(); }

	void GenerateNewPosition() {
		int gridX = rand() % num_cells;
		int gridY = rand() % num_cells;
		pos = { static_cast<float>(gridX * cell_size), static_cast<float>(gridY * cell_size) };
	}

	void Draw() {
		DrawRectangle(pos.x, pos.y, cell_size, cell_size, RED);
	}
};

class Snake {
public:
	std::vector<Vector2> body;
	Vector2 direction = { 1, 0 };
	bool alive = true;

	Snake() {
		body.push_back({ static_cast<float>(num_cells / 2 * cell_size), static_cast<float>(num_cells / 2 * cell_size) });
	}

	void Move() {
		Vector2 newHead = { body[0].x + direction.x * cell_size, body[0].y + direction.y * cell_size };
		body.insert(body.begin(), newHead);
		body.pop_back();
	}

	void Grow() {
		Vector2 newHead = { body[0].x + direction.x * cell_size, body[0].y + direction.y * cell_size };
		body.insert(body.begin(), newHead);
	}

	void Draw() {
		for (const Vector2& segment : body) {
			DrawRectangle(segment.x, segment.y, cell_size, cell_size, DARKGREEN);
		}
	}

	bool CheckCollisionWithSelf() {
		for (int i = 1; i < body.size(); ++i) {
			if (body[0].x == body[i].x && body[0].y == body[i].y) {
				return true;
			}
		}
		return false;
	}

	bool CheckCollisionWithWall() {
		return (body[0].x < 0 || body[0].x >= screen_width || body[0].y < 0 || body[0].y >= screen_height);
	}
};

void RaylibGameLoop(Snake& snake, Food& food, float& elapsedTime) {
	elapsedTime += GetFrameTime();

	if (elapsedTime >= 0.2f) {
		elapsedTime = 0.0f;

		if (snake.alive) {
			snake.Move();

			// Check if the snake eats the food
			if (snake.body[0].x == food.pos.x && snake.body[0].y == food.pos.y) {
				snake.Grow();
				food.GenerateNewPosition();
			}

			// Check for collisions with itself or walls
			if (snake.CheckCollisionWithSelf() || snake.CheckCollisionWithWall()) {
				snake.alive = false;
			}
		}
	}
	BeginDrawing();
	ClearBackground(RAYWHITE);

	food.Draw();
	snake.Draw();

	EndDrawing();
}

#pragma endregion

int main() {
	torchInit();

	InitWindow(screen_width, screen_height, "Raylib Game");
	SetTargetFPS(60);

	Snake snake;
	Food food;
	float elapsedTime = 0.0f;

	while (!WindowShouldClose()) {
		RaylibGameLoop(snake, food, elapsedTime);

		startRecording();
		std::string direction = torchInference();

		std::cout << direction << std::endl;

		if (direction == "up" && snake.direction.y != 1) snake.direction = { 0, -1 };
		else if (direction == "down" && snake.direction.y != -1) snake.direction = { 0, 1 };
		else if (direction == "left" && snake.direction.x != 1) snake.direction = { -1, 0 };
		else if (direction == "right" && snake.direction.x != -1) snake.direction = { 1, 0 };


		if (IsKeyPressed(KEY_R)) {
			writeWavFile("recording.wav", buffer.data(), bufferSize * sizeof(short));
			std::cout << "Recording saved to 'recording.wav'." << std::endl;
		}

		if (!snake.alive) {
			DrawText("GAME OVER", screen_width / 2 - 50, screen_height / 2, 20, RED);
			break;
		}
	}

	CloseWindow();
	stopRecording();

	std::cout << "Game over!\n";
}