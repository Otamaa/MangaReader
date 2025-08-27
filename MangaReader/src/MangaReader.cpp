#include <SFML/Graphics.hpp>
#include <SFML/Window.hpp>
#include <SFML/System.hpp>

#include <iostream>
#include <filesystem>
#include <vector>
#include <string>
#include <algorithm>
#include <set>
#include <windows.h>
#include <commdlg.h>
#include <shlobj.h>
#include <objbase.h>
#include <codecvt>
#include <locale>
#include <future>
#include <fstream>
#include <map>
#include <shared_mutex>

// Define SFML_STATIC if not already defined (for static linking)
#ifndef SFML_STATIC
#define SFML_STATIC
#endif

#include <archive.h>
#include <archive_entry.h>

#include <webp/decode.h>
#include <CLI/CLI.hpp>

class ImageSizeMismatchHandler {
private:
	sf::Vector2u previousImageSize;
	sf::Vector2u currentImageSize;
	bool hasPreviousSize;
	bool hasCurrentSize;

	// Configurable thresholds
	static constexpr float SIZE_THRESHOLD = 0.3f;
	static constexpr float AREA_THRESHOLD = 0.4f;
	static constexpr float DIMENSION_THRESHOLD = 0.25f;
	static constexpr float ASPECT_RATIO_THRESHOLD = 0.15f;

public:
	ImageSizeMismatchHandler() : previousImageSize(0, 0), currentImageSize(0, 0), hasPreviousSize(false), hasCurrentSize(false) { }

	bool hasSignificantAspectRatioChange(sf::Vector2u size1, sf::Vector2u size2) {
		if (size1.x == 0 || size1.y == 0 || size2.x == 0 || size2.y == 0) return false;

		float aspectRatio1 = static_cast<float>(size1.x) / static_cast<float>(size1.y);
		float aspectRatio2 = static_cast<float>(size2.x) / static_cast<float>(size2.y);

		float aspectRatioDiff = std::abs(aspectRatio1 - aspectRatio2) / std::min(aspectRatio1, aspectRatio2);

		return aspectRatioDiff > ASPECT_RATIO_THRESHOLD;
	}

	bool hasOrientationChange(sf::Vector2u size1, sf::Vector2u size2) {
		if (size1.x == 0 || size1.y == 0 || size2.x == 0 || size2.y == 0) return false;

		bool isPortrait1 = size1.y > size1.x;
		bool isPortrait2 = size2.y > size2.x;

		return isPortrait1 != isPortrait2;
	}

	bool hasSignificantSizeChange(sf::Vector2u previousSize, sf::Vector2u currentSize) {
		if (previousSize.x == 0 || previousSize.y == 0 || currentSize.x == 0 || currentSize.y == 0)
		{
			return false;
		}

		float previousArea = static_cast<float>(previousSize.x * previousSize.y);
		float currentArea = static_cast<float>(currentSize.x * currentSize.y);
		float areaRatio = currentArea / previousArea;

		float widthRatio = static_cast<float>(currentSize.x) / static_cast<float>(previousSize.x);
		float heightRatio = static_cast<float>(currentSize.y) / static_cast<float>(previousSize.y);

		bool significantAreaChange = (areaRatio > (1.0f + AREA_THRESHOLD) || areaRatio < (1.0f - AREA_THRESHOLD));
		bool significantWidthChange = (widthRatio > (1.0f + DIMENSION_THRESHOLD) || widthRatio < (1.0f - DIMENSION_THRESHOLD));
		bool significantHeightChange = (heightRatio > (1.0f + DIMENSION_THRESHOLD) || heightRatio < (1.0f - DIMENSION_THRESHOLD));

		return significantAreaChange || significantWidthChange || significantHeightChange;
	}

	// NEW: Check if NEXT image would need zoom reset (preemptive check)
	bool wouldNextImageNeedReset(sf::Vector2u nextImageSize) {
		if (!hasCurrentSize || currentImageSize.x == 0 || currentImageSize.y == 0)
		{
			return false; // No current image to compare against
		}

		bool sizeChange = hasSignificantSizeChange(currentImageSize, nextImageSize);
		bool aspectRatioChange = hasSignificantAspectRatioChange(currentImageSize, nextImageSize);
		bool orientationChange = hasOrientationChange(currentImageSize, nextImageSize);

		return sizeChange || aspectRatioChange || orientationChange;
	}

	// Update current image size (call this when loading an image)
	void setCurrentImageSize(sf::Vector2u size) {
		previousImageSize = currentImageSize;
		hasPreviousSize = hasCurrentSize;

		currentImageSize = size;
		hasCurrentSize = true;
	}

	// Check if zoom should be reset for current image (original method)
	bool shouldResetZoom(sf::Vector2u newImageSize) {
		bool shouldReset = false;

		if (hasCurrentSize && (currentImageSize.x != 0 && currentImageSize.y != 0))
		{
			bool sizeChange = hasSignificantSizeChange(currentImageSize, newImageSize);
			bool aspectRatioChange = hasSignificantAspectRatioChange(currentImageSize, newImageSize);
			bool orientationChange = hasOrientationChange(currentImageSize, newImageSize);

			shouldReset = sizeChange || aspectRatioChange || orientationChange;
		}

		setCurrentImageSize(newImageSize);
		return shouldReset;
	}

	void reset() {
		previousImageSize = sf::Vector2u(0, 0);
		currentImageSize = sf::Vector2u(0, 0);
		hasPreviousSize = false;
		hasCurrentSize = false;
	}
};

class LockedMessageBox {
private:
	static HWND mainWindowHandle;
	static bool isMessageBoxActive;
	static LONG originalWindowStyle;
	static bool wasMaximized;
	static RECT originalWindowRect;

public:
	// Set the main window handle (call this in your constructor)
	static void setMainWindow(HWND hwnd) {
		mainWindowHandle = hwnd;
	}

	// Check if message box is currently active
	static bool isActive() {
		return isMessageBoxActive;
	}

	// Lock the main window completely (disable moving, resizing, etc.)
	static void lockMainWindow() {
		if (!mainWindowHandle) return;

		// Store original window state
		wasMaximized = IsZoomed(mainWindowHandle);
		GetWindowRect(mainWindowHandle, &originalWindowRect);

		// Get current window style and store it
		originalWindowStyle = GetWindowLongW(mainWindowHandle, GWL_STYLE);

		// Remove resizing, moving, and system menu capabilities
		LONG newStyle = originalWindowStyle;
		newStyle &= ~(WS_SIZEBOX | WS_MAXIMIZEBOX | WS_MINIMIZEBOX);
		newStyle &= ~WS_SYSMENU; // Remove system menu (prevents moving via title bar)

		SetWindowLongW(mainWindowHandle, GWL_STYLE, newStyle);

		// Disable the window completely
		EnableWindow(mainWindowHandle, FALSE);

		// Force window to stay in place
		SetWindowPos(mainWindowHandle, HWND_NOTOPMOST,
			originalWindowRect.left, originalWindowRect.top,
			originalWindowRect.right - originalWindowRect.left,
			originalWindowRect.bottom - originalWindowRect.top,
			SWP_NOSIZE | SWP_NOMOVE | SWP_FRAMECHANGED);
	}

	// Unlock the main window and restore original state
	static void unlockMainWindow() {
		if (!mainWindowHandle) return;

		// Restore original window style
		SetWindowLongW(mainWindowHandle, GWL_STYLE, originalWindowStyle);

		// Re-enable the window
		EnableWindow(mainWindowHandle, TRUE);

		// Restore window position and size
		if (wasMaximized)
		{
			ShowWindow(mainWindowHandle, SW_MAXIMIZE);
		}
		else
		{
			SetWindowPos(mainWindowHandle, HWND_NOTOPMOST,
				originalWindowRect.left, originalWindowRect.top,
				originalWindowRect.right - originalWindowRect.left,
				originalWindowRect.bottom - originalWindowRect.top,
				SWP_FRAMECHANGED);
		}

		// Bring window back to front
		SetForegroundWindow(mainWindowHandle);
		SetActiveWindow(mainWindowHandle);
	}

	// Enhanced MessageBox that locks everything and stays on top
	static int showMessageBox(const std::wstring& message, const std::wstring& title, UINT type = MB_OK | MB_ICONINFORMATION) {
		isMessageBoxActive = true;

		// Completely lock the main window
		lockMainWindow();

		int result;

		// Force message box to stay on top with all possible flags
		UINT flags = type | MB_TASKMODAL | MB_TOPMOST | MB_SETFOREGROUND | MB_SYSTEMMODAL;

		if (mainWindowHandle)
		{
			// Show message box as modal to main window but force it to top
			result = MessageBoxW(mainWindowHandle, message.c_str(), title.c_str(), flags);
		}
		else
		{
			// Fallback with maximum priority
			result = MessageBoxW(NULL, message.c_str(), title.c_str(), flags);
		}

		// Unlock the main window and restore state
		unlockMainWindow();

		isMessageBoxActive = false;
		return result;
	}

	// Convenience methods for different message types
	static int showError(const std::wstring& message, const std::wstring& title = L"Error") {
		return showMessageBox(message, title, MB_OK | MB_ICONERROR);
	}

	static int showWarning(const std::wstring& message, const std::wstring& title = L"Warning") {
		return showMessageBox(message, title, MB_OK | MB_ICONWARNING);
	}

	static int showInfo(const std::wstring& message, const std::wstring& title = L"Information") {
		return showMessageBox(message, title, MB_OK | MB_ICONINFORMATION);
	}

	static int showQuestion(const std::wstring& message, const std::wstring& title = L"Question") {
		return showMessageBox(message, title, MB_YESNO | MB_ICONQUESTION);
	}
};

class NavigationLockManager {
private:
	std::atomic<bool> isLocked;
	std::string currentOperation;
	std::mutex lockMutex;

public:
	NavigationLockManager() : isLocked(false), currentOperation("") { }

	// Lock navigation with operation description
	void lock(const std::string& operation = "Loading") {
		std::lock_guard<std::mutex> guard(lockMutex);
		isLocked = true;
		currentOperation = operation;
	}

	// Unlock navigation
	void unlock() {
		std::lock_guard<std::mutex> guard(lockMutex);
		isLocked = false;
		currentOperation = "";
	}

	// Check if navigation is currently locked
	bool isNavigationLocked() const {
		return isLocked.load();
	}

	// Check if navigation is allowed (includes message box check)
	bool isNavigationAllowed() const {
		return !isLocked.load() && !LockedMessageBox::isActive();
	}

	// Get current operation description
	std::string getCurrentOperation() const {
		std::lock_guard<std::mutex> guard(const_cast<std::mutex&>(lockMutex));
		return currentOperation;
	}

	// Force unlock (emergency use)
	void forceUnlock() {
		isLocked = false;
		currentOperation = "";
	}
};

HWND LockedMessageBox::mainWindowHandle = NULL;
bool LockedMessageBox::isMessageBoxActive = false;
LONG LockedMessageBox::originalWindowStyle = 0;
bool LockedMessageBox::wasMaximized = false;
RECT LockedMessageBox::originalWindowRect = { 0, 0, 0, 0 };

// Unicode utility functions
class UnicodeUtils {
public:

	static std::string trim(const std::string& str) {
		size_t first = str.find_first_not_of(' ');
		if (first == std::string::npos) return "";
		size_t last = str.find_last_not_of(' ');
		return str.substr(first, (last - first + 1));
	}

	static std::wstring stringToWstring(const std::string& str) {
		if (str.empty()) return std::wstring();

		int sizeNeeded = MultiByteToWideChar(CP_UTF8, 0, str.c_str(), -1, NULL, 0);
		if (sizeNeeded <= 0) return std::wstring();

		std::wstring result(sizeNeeded - 1, 0);
		MultiByteToWideChar(CP_UTF8, 0, str.c_str(), -1, &result[0], sizeNeeded);
		return result;
	}

	static std::string wstringToString(const std::wstring& wstr) {
		if (wstr.empty()) return std::string();

		int sizeNeeded = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1, NULL, 0, NULL, NULL);
		if (sizeNeeded <= 0) return std::string();

		std::string result(sizeNeeded - 1, 0);
		WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1, &result[0], sizeNeeded, NULL, NULL);
		return result;
	}

	static sf::String stringToSFString(const std::string& str) {
		std::wstring wstr = stringToWstring(str);
		return sf::String(wstr);
	}

	static std::string getFilenameOnly(const std::string& path) {
		std::filesystem::path p(stringToWstring(path));
		return wstringToString(p.filename().wstring());
	}
};

class sf_text_wrapper {
private:
	std::unique_ptr<sf::Text> text;
public:

	sf_text_wrapper() = default;

	// Delete copy constructor and copy assignment (unique_ptr is not copyable)
	sf_text_wrapper(const sf_text_wrapper&) = delete;
	sf_text_wrapper& operator=(const sf_text_wrapper&) = delete;

	// Add move constructor and move assignment
	sf_text_wrapper(sf_text_wrapper&&) = default;
	sf_text_wrapper& operator=(sf_text_wrapper&&) = default;

	void initialize(const sf::Font& font, unsigned int size) {
		text = std::make_unique<sf::Text>(font, "null", size);
	}

	~sf_text_wrapper() = default;

	sf::Text* get() { return this->text.get(); };
};

class sf_Sprite_wrapper {
private:
	std::unique_ptr<sf::Sprite> sprite;
public:

	sf_Sprite_wrapper() = default;

	void initialize(const sf::Texture& texture) {
		sprite = (std::make_unique<sf::Sprite>(texture));
	}

	~sf_Sprite_wrapper() = default;

	sf::Sprite* get() { return this->sprite.get(); };
};

class sf_font_wrapper {
private:
	std::unique_ptr<sf::Font> font;
public:

	sf_font_wrapper() {
		font = std::make_unique<sf::Font>();

		try
		{
			if (!font->openFromFile("C:/Windows/Fonts/arial.ttf")) {
				if (!font->openFromFile("C:/Windows/Fonts/calibri.ttf")) {
					if (!font->openFromFile("C:/Windows/Fonts/segoeui.ttf")) {
						throw std::runtime_error("");
					}
				}
			}
		} catch (const std::exception& e)
		{
			LockedMessageBox::showError(
				L"Error: Could not load system font. Text may not display correctly.",
				L"Font Loading Error");
		}
	}

	~sf_font_wrapper() = default;

	sf::Font* get() { return this->font.get(); };
};

std::string wrapText(const std::string& str, const sf::Font& font, unsigned int charSize, float maxWidth) {
	std::istringstream words(str);
	std::string word;
	std::string wrapped;
	sf_text_wrapper temp;
	temp.initialize(font, charSize);

	float lineWidth = 0.f;

	while (words >> word)
	{
		std::string testLine = word + " ";
		temp.get()->setString(testLine);

		float wordWidth = temp.get()->getLocalBounds().size.x;

		if (lineWidth + wordWidth > maxWidth)
		{
			wrapped += "\n";  // start new line
			lineWidth = 0.f;
		}

		wrapped += word + " ";
		lineWidth += wordWidth;
	}

	return wrapped;
}

struct ArchiveEntry {
	std::string name;
	size_t size;
	int index; // Index in archive
};

//TODO : L".tif", L".tiff"
static constexpr std::array<const char*, 7u> supportedExtensions = {
	".jpg", ".jpeg", ".png", ".bmp", ".tga", ".gif", ".webp"
};

static constexpr std::array<const char*, 8u> supportedArchives = {
	 ".zip", ".cbz", ".rar", ".cbr", ".7z", ".cb7", ".tar", ".gz"
};

bool IsImgExtValid(const std::string& ext) {
	std::string cpy = ext;
	std::transform(cpy.begin(), cpy.end(), cpy.begin(), ::tolower);
	for (auto& supp : supportedExtensions)
	{
		if (cpy == supp)
		{
			return true;
		}
	}

	return false;
}

class ErrorDisplayHelper {
public:
	enum class ErrorType {
		CRITICAL,
		WARNING,
		INFO,
		MEMORY,
		CORRUPTION
	};

	struct ErrorContext {
		std::wstring archivePath;
		std::string operation;
		std::string details;
		size_t memorySize = 0;
		int entryIndex = -1;
		std::string fileName;

		ErrorContext& setArchive(const std::wstring& path) { archivePath = path; return *this; }
		ErrorContext& setOperation(const std::string& op) { operation = op; return *this; }
		ErrorContext& setDetails(const std::string& det) { details = det; return *this; }
		ErrorContext& setMemorySize(size_t size) { memorySize = size; return *this; }
		ErrorContext& setEntry(int index, const std::string& file) {
			entryIndex = index;
			fileName = file;
			return *this;
		}
	};

	static void showError(ErrorType type, const ErrorContext& context) {
		std::wstring message;
		std::wstring title;

		switch (type)
		{
		case ErrorType::CRITICAL:
			message = L"CRITICAL ARCHIVE ERROR\n\n";
			title = L"Archive Error";
			break;
		case ErrorType::WARNING:
			message = L"ARCHIVE ERROR (Skipping):\n\n";
			title = L"Archive Skipped";
			break;
		case ErrorType::MEMORY:
			message = L"MEMORY ERROR\n\n";
			title = L"Memory Error";
			appendMemoryInfo(message, context);
			break;
		case ErrorType::CORRUPTION:
			message = L"IMAGE CORRUPTION DETECTED\n\n";
			title = L"Image Corruption";
			appendCorruptionInfo(message, context);
			break;
		}

		// Common fields
		if (!context.archivePath.empty())
		{
			message += L"Archive: " + context.archivePath + L"\n";
		}
		if (!context.operation.empty())
		{
			message += L"Operation: " + UnicodeUtils::stringToWstring(context.operation) + L"\n";
		}
		if (!context.details.empty())
		{
			message += L"Error: " + UnicodeUtils::stringToWstring(context.details) + L"\n\n";
		}

		// Type-specific suffixes
		switch (type)
		{
		case ErrorType::CRITICAL:
			message += L"This archive may be corrupted or incompatible.";
			LockedMessageBox::showError(message, title);
			break;
		case ErrorType::WARNING:
			message += L"This archive will be skipped and the next one will be tried.";
			LockedMessageBox::showWarning(message, title);
			break;
		default:
			LockedMessageBox::showWarning(message, title);
			break;
		}
	}

private:
	static void appendMemoryInfo(std::wstring& message, const ErrorContext& context) {
		if (context.memorySize > 0)
		{
			message += L"Requested Size: " + std::to_wstring(context.memorySize / 1024 / 1024) + L" MB\n\n";
		}

		MEMORYSTATUSEX memInfo;
		memInfo.dwLength = sizeof(MEMORYSTATUSEX);
		if (GlobalMemoryStatusEx(&memInfo))
		{
			message += L"Available Memory: " + std::to_wstring(memInfo.ullAvailPhys / 1024 / 1024) + L" MB\n";
			message += L"Total Memory: " + std::to_wstring(memInfo.ullTotalPhys / 1024 / 1024) + L" MB\n\n";
		}

		message += L"The image is too large or system is low on memory.\n";
		message += L"Try closing other applications or skip this image.";
	}

	static void appendCorruptionInfo(std::wstring& message, const ErrorContext& context) {
		if (!context.fileName.empty())
		{
			message += L"Image: " + UnicodeUtils::stringToWstring(context.fileName) + L"\n";
		}
		if (context.entryIndex >= 0)
		{
			message += L"Entry Index: " + std::to_wstring(context.entryIndex) + L"\n\n";
		}
		message += L"This image appears to be corrupted and will be skipped.";
	}
};

class ImageLoader {
public:
	struct LoadResult {
		sf::Image image;
		bool success;
		std::string errorMessage;

		LoadResult() : image() , success(false) { }
		LoadResult(sf::Image img) : image(std::move(img)), success(true) { }
		LoadResult(const std::string& error) : image(), success(false), errorMessage(error) { }
	};

	// Unified image loading from file or memory
	static LoadResult loadImage(const std::wstring& filePath) {
		try
		{
			sf::Image image;
			std::string filename = UnicodeUtils::wstringToString(filePath);

			if (isWebPFile(filename))
			{
				if (loadWebPFromFile(filePath, image))
				{
					return LoadResult(std::move(image));
				}
			}
			else
			{
				if (image.loadFromFile(filePath))
				{
					return LoadResult(std::move(image));
				}
			}
			return LoadResult("Failed to load image: " + filename);
		} catch (const std::exception& e)
		{
			return LoadResult("Exception loading image: " + std::string(e.what()));
		}
	}

	static LoadResult loadImageFromMemory(const std::vector<uint8_t>& data, const std::string& filename) {
		try
		{
			sf::Image image;

			if (isWebPFile(filename))
			{
				if (loadWebPFromMemory(data, image))
				{
					return LoadResult(std::move(image));
				}
			}
			else
			{
				if (image.loadFromMemory(data.data(), data.size()))
				{
					return LoadResult(std::move(image));
				}
			}
			return LoadResult("Failed to decode image data: " + filename);
		} catch (const std::exception& e)
		{
			return LoadResult("Exception decoding image: " + std::string(e.what()));
		}
	}

	static bool isWebPFile(const std::string& filename) {
		std::filesystem::path path(filename);
		std::string extension = path.extension().string();
		std::transform(extension.begin(), extension.end(), extension.begin(), ::tolower);
		return extension == ".webp";
	}

	// These would be your existing WebP loading functions - kept here for completeness
	static bool loadWebPFromFile(const std::wstring& filePath, sf::Image& image, int sizeLimit = 100 * 1024 * 1024) {

		try
		{
			std::ifstream file(filePath, std::ios::binary | std::ios::ate);

			if (!file.is_open() || !file.good())
			{
				throw std::runtime_error("Failed to open WebP file: " + UnicodeUtils::wstringToString(filePath));
			}

			std::streamsize size = file.tellg();
			if (size <= 0)
			{
				file.close();
				throw std::runtime_error("WebP file is empty or invalid size: " + UnicodeUtils::wstringToString(filePath));
			}

			if (size >= sizeLimit)
			{
				file.close();
				throw std::runtime_error("WebP file too large (>100MB): " + UnicodeUtils::wstringToString(filePath));
			}

			file.seekg(0, std::ios::beg);
			if (!file.good())
			{
				file.close();
				throw std::runtime_error("Failed to seek in WebP file: " + UnicodeUtils::wstringToString(filePath));
			}

			std::vector<uint8_t> buffer(static_cast<size_t>(size));
			if (!file.read(reinterpret_cast<char*>(buffer.data()), size) || file.gcount() != size)
			{
				file.close();
				throw std::runtime_error("Failed to read WebP file completely: " + UnicodeUtils::wstringToString(filePath));
			}

			file.close();

			if (!loadWebPFromMemory(buffer, image))
			{
				throw std::runtime_error("Failed to decode WebP data: " + UnicodeUtils::wstringToString(filePath));
			}

			return true;

		} catch (const std::exception& e)
		{
			throw std::runtime_error("WebP loading error: " + std::string(e.what()));
		}
	}

	static bool loadWebPFromMemory(const std::vector<uint8_t>& data, sf::Image& image) {
		int width, height;

		uint8_t* decoded = WebPDecodeRGBA(data.data(), data.size(), &width, &height);

		if (!decoded)
		{
			return false;
		}

		image = sf::Image(sf::Vector2u(width, height), decoded);
		WebPFree(decoded);

		return true;
	}

};

class NavigationHelper {
public:
	static bool canNavigate(const NavigationLockManager& navLock) {
		return navLock.isNavigationAllowed() && !LockedMessageBox::isActive();
	}

	static bool executeIfNavigationAllowed(const NavigationLockManager& navLock,
		const std::function<void()>& action) {
		if (canNavigate(navLock))
		{
			action();
			return true;
		}
		return false;
	}
};

class FileSystemHelper {
public:
	static std::string getFileSizeString(size_t fileSize) {
		if (fileSize < 1024)
		{
			return std::to_string(fileSize) + " B";
		}
		else if (fileSize < 1024 * 1024)
		{
			return std::to_string(fileSize / 1024) + " KB";
		}
		else
		{
			return std::to_string(fileSize / (1024 * 1024)) + " MB";
		}
	}

	static std::string getFileSizeString(const std::wstring& filePath) {
		try
		{
			auto fileSize = std::filesystem::file_size(filePath);
			return getFileSizeString(static_cast<size_t>(fileSize));
		} catch (...)
		{
			return "Unknown";
		}
	}

	static std::string extractFilenameFromPath(const std::wstring& path, bool isArchive = false) {
		if (isArchive)
		{
			std::string filename = UnicodeUtils::wstringToString(path);
			size_t hashPos = filename.find('#');
			if (hashPos != std::string::npos)
			{
				filename = filename.substr(hashPos + 1);
			}
			std::filesystem::path p(filename);
			return p.filename().string();
		}
		else
		{
			return UnicodeUtils::getFilenameOnly(UnicodeUtils::wstringToString(path));
		}
	}
};

struct PathLimitChecker {

	static bool isRunningAsAdmin() {
		BOOL isAdmin = FALSE;
		HANDLE hToken = NULL;

		if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &hToken))
		{
			TOKEN_ELEVATION elevation;
			DWORD cbSize = sizeof(TOKEN_ELEVATION);

			if (GetTokenInformation(hToken, TokenElevation, &elevation, sizeof(elevation), &cbSize))
			{
				isAdmin = elevation.TokenIsElevated;
			}
			CloseHandle(hToken);
		}

		return isAdmin == TRUE;
	}

	static bool enableLongPathSupport() {
		if (!isRunningAsAdmin())
		{
			return false; // Need admin privileges
		}

		HKEY hKey;
		LONG result = RegOpenKeyExW(
			HKEY_LOCAL_MACHINE,
			L"SYSTEM\\CurrentControlSet\\Control\\FileSystem",
			0,
			KEY_SET_VALUE,
			&hKey
		);

		if (result == ERROR_SUCCESS)
		{
			DWORD value = 1;
			result = RegSetValueExW(
				hKey,
				L"LongPathsEnabled",
				0,
				REG_DWORD,
				reinterpret_cast<const BYTE*>(&value),
				sizeof(DWORD)
			);

			RegCloseKey(hKey);
			return (result == ERROR_SUCCESS);
		}

		return false;
	}

	static bool isLongPathSupportEnabled() {
		HKEY hKey;
		DWORD dwValue = 0;
		DWORD dwSize = sizeof(DWORD);

		// Open the registry key
		LONG result = RegOpenKeyExW(
			HKEY_LOCAL_MACHINE,
			L"SYSTEM\\CurrentControlSet\\Control\\FileSystem",
			0,
			KEY_READ,
			&hKey
		);

		if (result == ERROR_SUCCESS)
		{
			// Query the LongPathsEnabled value
			result = RegQueryValueExW(
				hKey,
				L"LongPathsEnabled",
				nullptr,
				nullptr,
				reinterpret_cast<LPBYTE>(&dwValue),
				&dwSize
			);

			RegCloseKey(hKey);

			if (result == ERROR_SUCCESS)
			{
				return (dwValue == 1);
			}
		}

		return false; // Default to disabled if can't read
	}

	static void showPathLimitInfo() {
		bool longPathsEnabled = PathLimitChecker::isLongPathSupportEnabled();
		bool isAdmin = PathLimitChecker::getIsRunningAsAdmin();

		std::wstring message = L"PATH LENGTH INFORMATION:\n\n";
		message += L"Current Path Limit: " + std::to_wstring(PathLimitChecker::getMaxPathLength()) + L" characters\n";
		message += L"Long Path Support: " + std::wstring(longPathsEnabled ? L"ENABLED" : L"DISABLED") + L"\n";
		message += L"Running as Administrator: " + std::wstring(isAdmin ? L"YES" : L"NO") + L"\n\n";

		if (!longPathsEnabled)
		{
			if (isAdmin)
			{
				message += L"Long path support can be enabled automatically.\n";
				message += L"Click 'YES' to enable it now, or 'NO' for manual instructions.";

				int result = LockedMessageBox::showQuestion(message, L"Enable Long Path Support?");

				if (result == IDYES)
				{
					if (PathLimitChecker::tryEnableLongPaths())
					{
						LockedMessageBox::showInfo(
							L"Long path support has been enabled successfully!\n\n"
							L"Note: You may need to restart the application for changes to take full effect.",
							L"Success"
						);
					}
					else
					{
						LockedMessageBox::showError(
							L"Failed to enable long path support.\n"
							L"Please enable it manually using the instructions below.",
							L"Enable Failed"
						);
						showManualInstructions();
					}
				}
				else
				{
					showManualInstructions();
				}
			}
			else
			{
				message += L"To enable long path support, administrator privileges are required.\n";
				message += L"Click 'YES' to restart as administrator, or 'NO' for manual instructions.";

				int result = LockedMessageBox::showQuestion(message, L"Restart as Administrator?");

				if (result == IDYES)
				{
					if (PathLimitChecker::restartAsAdmin())
					{
						// Application will restart with elevation
						std::exit(0);
					}
					else
					{
						LockedMessageBox::showError(
							L"Failed to restart with administrator privileges.\n"
							L"Please run the application as administrator manually.",
							L"Elevation Failed"
						);
					}
				}
				else
				{
					showManualInstructions();
				}
			}
		}
		else
		{
			LockedMessageBox::showInfo(message, L"Path Length Settings");
		}
	}

	static void showManualInstructions() {
		std::wstring message = L"MANUAL SETUP INSTRUCTIONS:\n\n";
		message += L"Method 1 - Group Policy Editor:\n";
		message += L"1. Press Win+R, type 'gpedit.msc', press Enter\n";
		message += L"2. Navigate to: Computer Configuration > Administrative Templates > System > Filesystem\n";
		message += L"3. Double-click 'Enable NTFS long paths'\n";
		message += L"4. Select 'Enabled', click OK\n";
		message += L"5. Restart this application\n\n";

		message += L"Method 2 - Registry Editor:\n";
		message += L"1. Press Win+R, type 'regedit', press Enter\n";
		message += L"2. Navigate to: HKEY_LOCAL_MACHINE\\SYSTEM\\CurrentControlSet\\Control\\FileSystem\n";
		message += L"3. Create or modify DWORD: LongPathsEnabled\n";
		message += L"4. Set value to: 1\n";
		message += L"5. Restart this application\n\n";

		message += L"Method 3 - Command Line (Run as Administrator):\n";
		message += L"reg add \"HKLM\\SYSTEM\\CurrentControlSet\\Control\\FileSystem\" /v LongPathsEnabled /t REG_DWORD /d 1";

		LockedMessageBox::showInfo(message, L"Manual Setup Instructions");
	}

	static size_t getMaxComponentLength() {
		// Component length limit doesn't change
		return 255;
	}

	static size_t getMaxPathLength() {
		if (isLongPathSupportEnabled())
		{
			return 32767;
		}
		else
		{
			return 260;
		}
	}

	static size_t getSafePathLength() {
		size_t maxPath = getMaxPathLength();
		return maxPath > 260 ? maxPath - 50 : 240;
	}

	// Try to enable long path support
	static bool tryEnableLongPaths() {
		if (isLongPathSupportEnabled())
		{
			return true; // Already enabled
		}

		if (isRunningAsAdmin())
		{
			return enableLongPathSupport();
		}

		return false; // Need elevation
	}

	// Restart application with admin privileges
	static bool restartAsAdmin() {
		wchar_t exePath[MAX_PATH];
		GetModuleFileNameW(NULL, exePath, MAX_PATH);

		// Preserve current command line arguments and add --enable-long-paths
		std::wstring params = L"--enable-long-paths";

		// Add other relevant arguments if needed
		// You could store original argc/argv to reconstruct them here

		SHELLEXECUTEINFOW sei = {};
		sei.cbSize = sizeof(SHELLEXECUTEINFOW);
		sei.lpVerb = L"runas";
		sei.lpFile = exePath;
		sei.lpParameters = params.c_str();
		sei.nShow = SW_NORMAL;
		sei.fMask = SEE_MASK_NOCLOSEPROCESS;

		return ShellExecuteExW(&sei) == TRUE;
	}

	static bool getIsRunningAsAdmin() {
		return isRunningAsAdmin();
	}


	static void handleEnableLongPaths() {

		if (PathLimitChecker::isLongPathSupportEnabled())
		{
			LockedMessageBox::showInfo(
				L"Long path support is already enabled.\n"
				L"Maximum path length: " + std::to_wstring(PathLimitChecker::getMaxPathLength()) + L" characters",
				L"Already Enabled"
			);
			return;
		}

		if (!PathLimitChecker::getIsRunningAsAdmin())
		{
			LockedMessageBox::showError(
				L"Administrator privileges required to enable long path support.\n"
				L"Please run the application as administrator with --enable-long-paths flag.",
				L"Admin Required"
			);
			return;
		}

		if (PathLimitChecker::tryEnableLongPaths())
		{
			LockedMessageBox::showInfo(
				L"Long path support has been enabled successfully!\n\n"
				L"New maximum path length: " + std::to_wstring(PathLimitChecker::getMaxPathLength()) + L" characters\n"
				L"The application will now start with long path support.",
				L"Long Paths Enabled"
			);
		}
		else
		{
			LockedMessageBox::showError(
				L"Failed to enable long path support.\n"
				L"Please try enabling it manually through Group Policy or Registry Editor.",
				L"Enable Failed"
			);
		}
	}

	static void showPathInfoConsole() {
		// For console output (useful for command line usage)
		std::wcout << L"Path Length Information:\n";
		std::wcout << L"Current Limit: " << PathLimitChecker::getMaxPathLength() << L" characters\n";
		std::wcout << L"Long Path Support: " << (PathLimitChecker::isLongPathSupportEnabled() ? L"ENABLED" : L"DISABLED") << L"\n";
		std::wcout << L"Running as Admin: " << (PathLimitChecker::getIsRunningAsAdmin() ? L"YES" : L"NO") << L"\n";

		if (!PathLimitChecker::isLongPathSupportEnabled())
		{
			std::wcout << L"\nTo enable long path support, run with administrator privileges:\n";
			std::wcout << L"manga_reader.exe --enable-long-paths\n";
		}
	}

};

class ArchiveHandler {
private:
	struct archive* archive;
	std::string archivePath;
	std::wstring archivePathW;
	std::vector<ArchiveEntry> imageEntries;
	std::vector<std::vector<uint8_t>> cachedImages; // Cache extracted images
	bool isArchiveOpen;  // Add state tracking
	std::mutex archiveMutex;
	std::set<int> corruptedEntries;

public:
	ArchiveHandler() : archive(nullptr), archivePath(), archivePathW(), imageEntries(), cachedImages(), isArchiveOpen(false), archiveMutex(), corruptedEntries() { }

	~ArchiveHandler() {
		closeArchive();
	}

	bool openArchive(const std::wstring& path) {
		std::lock_guard<std::mutex> lock(archiveMutex);

		archivePathW = path;

		// Run compatibility test first - if fails, return false to skip
		if (!testArchiveCompatibility())
		{
			return false; // This will cause the caller to skip to next archive
		}

		try
		{
			// Always close existing archive first
			closeArchiveInternal();

			archivePath = UnicodeUtils::wstringToString(path);

			// Check if file exists and is readable
			if (!std::filesystem::exists(path))
			{
				ErrorDisplayHelper::showError(ErrorDisplayHelper::ErrorType::CRITICAL,
					ErrorDisplayHelper::ErrorContext()
					.setArchive(archivePathW)
					.setOperation("File Check")
					.setDetails("Archive file does not exist: " + archivePath));
				return false;
			}

			// Check file size
			auto fileSize = std::filesystem::file_size(path);
			if (fileSize == 0)
			{
				ErrorDisplayHelper::showError(ErrorDisplayHelper::ErrorType::CRITICAL,
					ErrorDisplayHelper::ErrorContext()
					.setArchive(archivePathW)
					.setOperation("File Check")
					.setDetails("Archive file is empty"));
				return false;
			}

			archive = archive_read_new();
			if (!archive)
			{
				ErrorDisplayHelper::showError(ErrorDisplayHelper::ErrorType::CRITICAL,
					ErrorDisplayHelper::ErrorContext()
					.setArchive(archivePathW)
					.setOperation("Archive Creation")
					.setDetails("Failed to create archive object"));
				return false;
			}

			// Enable all supported formats and filters
			archive_read_support_filter_all(archive);
			archive_read_support_format_all(archive);
			archive_read_set_option(archive, NULL, "hdrcharset", "UTF-8");

			std::string pathStr = UnicodeUtils::wstringToString(path);
			std::string ext = std::filesystem::path(pathStr).extension().string();
			std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

			// Format-specific optimizations
			if (ext == ".rar" || ext == ".cbr")
			{
				archive_read_set_option(archive, "rar", "hdrcharset", "UTF-8");
				archive_read_set_option(archive, "rar", "pwdfile", NULL);
			}
			else if (ext == ".7z" || ext == ".cb7")
			{
				archive_read_set_option(archive, "7zip", "hdrcharset", "UTF-8");
			}
			else if (ext == ".tar" || ext == ".gz")
			{
				archive_read_set_option(archive, "tar", "hdrcharset", "UTF-8");
			}

			int result = archive_read_open_filename_w(archive, path.c_str(), 10240);
			if (result != ARCHIVE_OK)
			{
				std::string errorMsg = "Failed to open archive";
				if (archive_error_string(archive))
				{
					errorMsg += "\nLibarchive error: " + std::string(archive_error_string(archive));
				}
				ErrorDisplayHelper::showError(ErrorDisplayHelper::ErrorType::CRITICAL,
					ErrorDisplayHelper::ErrorContext()
					.setArchive(archivePathW)
					.setOperation("Archive Opening")
					.setDetails(errorMsg));
				closeArchiveInternal();
				return false;
			}

			isArchiveOpen = true;

			if (!loadImageEntries())
			{
				closeArchiveInternal();
				return false;
			}

			return true;

		} catch (const std::filesystem::filesystem_error& e)
		{
			ErrorDisplayHelper::showError(ErrorDisplayHelper::ErrorType::CRITICAL,
				ErrorDisplayHelper::ErrorContext()
				.setArchive(archivePathW)
				.setOperation("Filesystem Error")
				.setDetails(e.what()));
			closeArchiveInternal();
			return false;
		} catch (const std::exception& e)
		{
			ErrorDisplayHelper::showError(ErrorDisplayHelper::ErrorType::CRITICAL,
				ErrorDisplayHelper::ErrorContext()
				.setArchive(archivePathW)
				.setOperation("Exception in openArchive")
				.setDetails(e.what()));
			closeArchiveInternal();
			return false;
		} catch (...)
		{
			ErrorDisplayHelper::showError(ErrorDisplayHelper::ErrorType::CRITICAL,
				ErrorDisplayHelper::ErrorContext()
				.setArchive(archivePathW)
				.setOperation("Unknown Exception")
				.setDetails("Unknown exception occurred in openArchive"));
			closeArchiveInternal();
			return false;
		}
	}

	void closeArchive() {
		std::lock_guard<std::mutex> lock(archiveMutex);
		closeArchiveInternal();
	}

	// Add method to preload next few images for smoother navigation
	void preloadImages(int currentIndex, int preloadCount = 2) {
		for (int i = 1; i <= preloadCount && (currentIndex + i) < imageEntries.size(); ++i)
		{
			int nextIndex = currentIndex + i;
			if (nextIndex < cachedImages.size() && cachedImages[nextIndex].empty())
			{
				std::vector<uint8_t> dummy;
				extractImageToMemory(nextIndex, dummy);
			}
		}
	}

	bool hasKnownIssues() const {
		return !corruptedEntries.empty();
	}

	std::string getCorruptionReport() const {
		if (corruptedEntries.empty()) return "";

		std::string report = "Corrupted entries in " + archivePath + ":\n";
		for (int index : corruptedEntries)
		{
			if (index < imageEntries.size())
			{
				report += "- Entry " + std::to_string(index) + ": " + imageEntries[index].name + "\n";
			}
		}
		return report;
	}

	const std::vector<ArchiveEntry>& getImageEntries() const {
		return imageEntries;
	}

	const bool getIsArchiveOpen() const {
		return isArchiveOpen;
	}

	void clearCache(int index = -1) {
		std::lock_guard<std::mutex> lock(archiveMutex);
		try
		{
			if (index >= 0 && index < static_cast<int>(cachedImages.size()))
			{
				cachedImages[index].clear();
				cachedImages[index].shrink_to_fit();
			}
			else if (index == -1)
			{
				for (auto& cache : cachedImages)
				{
					cache.clear();
					cache.shrink_to_fit();
				}
				cachedImages.clear();
			}
		} catch (const std::exception& e)
		{
			ErrorDisplayHelper::showError(ErrorDisplayHelper::ErrorType::CRITICAL,
				ErrorDisplayHelper::ErrorContext()
				.setArchive(archivePathW)
				.setOperation("clearCache")
				.setDetails(e.what()));
		}
	}

	bool isCached(int index) {
		std::lock_guard<std::mutex> lock(archiveMutex);
		return index >= 0 && index < static_cast<int>(cachedImages.size()) && !cachedImages[index].empty();
	}

	bool extractImageToMemory(int entryIndex, std::vector<uint8_t>& buffer) {
		std::lock_guard<std::mutex> lock(archiveMutex);

		try
		{
			if (!isArchiveOpen || entryIndex < 0 || entryIndex >= imageEntries.size())
			{
				std::string error = "Invalid extraction parameters. Index: " + std::to_string(entryIndex) +
					", Archive open: " + (isArchiveOpen ? "true" : "false") +
					", Entries: " + std::to_string(imageEntries.size());
				ErrorDisplayHelper::showError(ErrorDisplayHelper::ErrorType::CRITICAL,
					ErrorDisplayHelper::ErrorContext()
					.setArchive(archivePathW)
					.setOperation("Parameter Validation")
					.setDetails(error));
				return false;
			}

			// Check if this entry was previously marked as corrupted
			if (corruptedEntries.find(entryIndex) != corruptedEntries.end())
			{
				std::wstring message = L"Skipping previously corrupted image:\n\n";
				message += L"Entry: " + std::to_wstring(entryIndex) + L"\n";
				message += L"File: " + UnicodeUtils::stringToWstring(imageEntries[entryIndex].name);

				LockedMessageBox::showError(message, L"Corrupted Image Skipped");
				return false;
			}

			// Check if image is already cached
			if (entryIndex < cachedImages.size() && !cachedImages[entryIndex].empty())
			{
				buffer = cachedImages[entryIndex];
				return true;
			}

			// Extract using the main archive instance with error checking
			if (!extractAndCacheImageInternal(entryIndex))
			{
				corruptedEntries.insert(entryIndex); // Mark as corrupted
				ErrorDisplayHelper::showError(ErrorDisplayHelper::ErrorType::CORRUPTION,
					ErrorDisplayHelper::ErrorContext()
					.setArchive(archivePathW)
					.setEntry(entryIndex, imageEntries[entryIndex].name));
				return false;
			}

			// Return cached data
			if (entryIndex < cachedImages.size() && !cachedImages[entryIndex].empty())
			{
				buffer = cachedImages[entryIndex];
				return true;
			}

			ErrorDisplayHelper::showError(ErrorDisplayHelper::ErrorType::CRITICAL,
				ErrorDisplayHelper::ErrorContext()
				.setArchive(archivePathW)
				.setOperation("Cache Error")
				.setDetails("Cache is empty after successful extraction for entry: " + std::to_string(entryIndex)));
			return false;

		} catch (const std::bad_alloc& e)
		{
			corruptedEntries.insert(entryIndex);
			ErrorDisplayHelper::showError(ErrorDisplayHelper::ErrorType::MEMORY,
				ErrorDisplayHelper::ErrorContext()
				.setArchive(archivePathW)
				.setOperation("extractImageToMemory")
				.setMemorySize(0));
			return false;
		} catch (const std::exception& e)
		{
			corruptedEntries.insert(entryIndex);
			ErrorDisplayHelper::showError(ErrorDisplayHelper::ErrorType::CRITICAL,
				ErrorDisplayHelper::ErrorContext()
				.setArchive(archivePathW)
				.setOperation("extractImageToMemory Exception")
				.setDetails("Entry: " + std::to_string(entryIndex) + " - " + e.what()));
			return false;
		} catch (...)
		{
			corruptedEntries.insert(entryIndex);
			ErrorDisplayHelper::showError(ErrorDisplayHelper::ErrorType::CRITICAL,
				ErrorDisplayHelper::ErrorContext()
				.setArchive(archivePathW)
				.setOperation("extractImageToMemory Unknown Exception")
				.setDetails("Unknown exception for entry: " + std::to_string(entryIndex)));
			return false;
		}
	}

	bool isSafeToAllocate(size_t requestedSize) {
		const size_t MAX_SINGLE_ALLOCATION = 200 * 1024 * 1024; // 200MB max per image
		const size_t MIN_FREE_MEMORY = 500 * 1024 * 1024; // Keep 500MB free

		if (requestedSize > MAX_SINGLE_ALLOCATION)
		{
			ErrorDisplayHelper::showError(ErrorDisplayHelper::ErrorType::MEMORY,
				ErrorDisplayHelper::ErrorContext()
				.setArchive(archivePathW)
				.setOperation("Size Check")
				.setMemorySize(requestedSize));
			return false;
		}

		MEMORYSTATUSEX memInfo;
		memInfo.dwLength = sizeof(MEMORYSTATUSEX);
		if (GlobalMemoryStatusEx(&memInfo))
		{
			if (memInfo.ullAvailPhys < (requestedSize + MIN_FREE_MEMORY))
			{
				ErrorDisplayHelper::showError(ErrorDisplayHelper::ErrorType::MEMORY,
					ErrorDisplayHelper::ErrorContext()
					.setArchive(archivePathW)
					.setOperation("Memory Check")
					.setMemorySize(requestedSize));
				return false;
			}
		}

		return true;
	}

	std::vector<uint8_t> safeAllocateVector(size_t size) {
		if (!isSafeToAllocate(size))
		{
			throw std::bad_alloc();
		}

		try
		{
			return std::vector<uint8_t>(size);
		} catch (const std::bad_alloc& e)
		{
			ErrorDisplayHelper::showError(ErrorDisplayHelper::ErrorType::CRITICAL,
				ErrorDisplayHelper::ErrorContext()
				.setArchive(archivePathW)
				.setOperation("Vector Allocation")
				.setDetails(e.what()));
			throw;
		}
	}

private:

	bool extractAndCacheImage(int targetIndex) {
		if (!archive || targetIndex < 0 || targetIndex >= imageEntries.size())
		{
			return false;
		}

		// Ensure cache is properly sized
		if (cachedImages.size() < imageEntries.size())
		{
			cachedImages.resize(imageEntries.size());
		}

		// If already cached, return success
		if (!cachedImages[targetIndex].empty())
		{
			return true;
		}

		// Create a new archive instance for extraction
		struct archive* extractArchive = archive_read_new();
		if (!extractArchive)
		{
			return false;
		}

		archive_read_support_filter_all(extractArchive);
		archive_read_support_format_all(extractArchive);
		archive_read_set_option(extractArchive, NULL, "hdrcharset", "UTF-8");

		// Open archive for extraction
		std::wstring wPath = UnicodeUtils::stringToWstring(archivePath);
		int result = archive_read_open_filename_w(extractArchive, wPath.c_str(), 10240);

		if (result != ARCHIVE_OK)
		{
			archive_read_free(extractArchive);
			return false;
		}

		struct archive_entry* entry;
		int currentIndex = 0;
		bool found = false;

		// Find and extract the target image
		while (archive_read_next_header(extractArchive, &entry) == ARCHIVE_OK)
		{
			const char* pathname = archive_entry_pathname(entry);
			if (!pathname)
			{
				archive_read_data_skip(extractArchive);
				continue;
			}

			std::string fileName;
			try
			{
				fileName = std::string(pathname);
				std::wstring wFileName = UnicodeUtils::stringToWstring(fileName);
				fileName = UnicodeUtils::wstringToString(wFileName);
			} catch (...)
			{
				fileName = std::string(pathname);
			}

			if (IsImgExtValid(std::filesystem::path(fileName).extension().string()))
			{
				if (currentIndex == targetIndex)
				{
					// Extract this image
					la_int64_t size = archive_entry_size(entry);
					if (size > 0)
					{
						cachedImages[targetIndex].resize(static_cast<size_t>(size));
						la_ssize_t bytesRead = archive_read_data(extractArchive,
							cachedImages[targetIndex].data(), cachedImages[targetIndex].size());

						if (bytesRead == size)
						{
							found = true;
						}
						else
						{
							cachedImages[targetIndex].clear();
						}
					}

					break;
				}
				else
				{
					archive_read_data_skip(extractArchive);
				}
				currentIndex++;
			}
			else
			{
				archive_read_data_skip(extractArchive);
			}
		}

		// Always clean up extraction archive
		archive_read_free(extractArchive);
		return found;
	}

	bool loadImageEntries() {
		try
		{
			imageEntries.clear();

			if (!archive || !isArchiveOpen) return false;

			struct archive_entry* entry;
			int index = 0;
			int totalEntries = 0;
			int maxFolderDepth = 0;
			size_t maxPathLength = 0;

			while (archive_read_next_header(archive, &entry) == ARCHIVE_OK)
			{
				totalEntries++;

				// Get entry info
				const char* pathname = archive_entry_pathname(entry);
				la_int64_t entry_type = archive_entry_filetype(entry);
				la_int64_t entry_size = archive_entry_size(entry);

				// Skip directories
				if (entry_type == AE_IFDIR)
				{
					archive_read_data_skip(archive);
					continue;
				}

				// Skip non-regular files
				if (entry_type != AE_IFREG)
				{
					archive_read_data_skip(archive);
					continue;
				}

				// Handle pathname
				std::string currentPath;
				if (!pathname)
				{
					currentPath = "unknown_" + std::to_string(index);
				}
				else
				{
					currentPath = std::string(pathname);
				}

				// Normalize path
				std::replace(currentPath.begin(), currentPath.end(), '\\', '/');

				// Calculate folder depth and path length
				int folderDepth = std::count(currentPath.begin(), currentPath.end(), '/');
				maxFolderDepth = std::max(maxFolderDepth, folderDepth);
				maxPathLength = std::max(maxPathLength, currentPath.length());

				// Check if it's an image file
				std::filesystem::path filePath(currentPath);
				std::string extension = filePath.extension().string();

				if (IsImgExtValid(extension) && entry_size > 0)
				{
					ArchiveEntry archiveEntryStruct;
					archiveEntryStruct.name = currentPath;
					archiveEntryStruct.size = static_cast<size_t>(entry_size);
					archiveEntryStruct.index = index++;

					imageEntries.push_back(archiveEntryStruct);
				}

				archive_read_data_skip(archive);
			}

			// Check if the internal structure is too complex
			const int MAX_FOLDER_DEPTH = 5;
			const size_t MAX_INTERNAL_PATH = 150;

			if (maxFolderDepth > MAX_FOLDER_DEPTH || maxPathLength > MAX_INTERNAL_PATH)
			{
				std::wstring message = L"ARCHIVE SKIPPED - COMPLEX STRUCTURE:\n\n";
				message += L"Max folder depth: " + std::to_wstring(maxFolderDepth) + L" (limit: " + std::to_wstring(MAX_FOLDER_DEPTH) + L")\n";
				message += L"Max internal path: " + std::to_wstring(maxPathLength) + L" chars (limit: " + std::to_wstring(MAX_INTERNAL_PATH) + L")\n";
				message += L"Images found: " + std::to_wstring(imageEntries.size()) + L"\n\n";
				message += L"Moving to next archive...";

				LockedMessageBox::showError(message, L"Archive Skipped - Complex Structure");
				return false;
			}

			// Sort entries by index to maintain order
			std::sort(imageEntries.begin(), imageEntries.end(),
				[](const ArchiveEntry& a, const ArchiveEntry& b) {
					return a.index < b.index;
				});

			if (imageEntries.empty())
			{
				std::wstring message = L"No images found in archive.\n";
				message += L"Moving to next archive...";
				LockedMessageBox::showError(message, L"No Images Found");
				return false;
			}

			return true;

		} catch (const std::exception& e)
		{
			ErrorDisplayHelper::showError(ErrorDisplayHelper::ErrorType::CRITICAL,
				ErrorDisplayHelper::ErrorContext()
				.setArchive(archivePathW)
				.setOperation("loadImageEntries Exception")
				.setDetails(e.what()));
			return false;
		} catch (...)
		{
			ErrorDisplayHelper::showError(ErrorDisplayHelper::ErrorType::CRITICAL,
				ErrorDisplayHelper::ErrorContext()
				.setArchive(archivePathW)
				.setOperation("loadImageEntries")
				.setDetails("Unknown exception occurred"));
			return false;
		}
	}

	void closeArchiveInternal() {
		if (archive)
		{
			archive_read_free(archive);
			archive = nullptr;
		}
		isArchiveOpen = false;
		archivePath.clear();
		imageEntries.clear();
		cachedImages.clear();
		corruptedEntries.clear();
	}

	bool testArchiveCompatibility() {
		std::string archiveName = UnicodeUtils::wstringToString(archivePathW);

		std::filesystem::path archivePath(archiveName);
		std::string archiveFilename = archivePath.filename().string();
		std::string archiveDir = archivePath.parent_path().string();

		size_t archivePathLength = archiveName.length();
		size_t basePathLength = archiveDir.length();

		// Dynamic limits based on system configuration
		const size_t MAX_TOTAL_PATH = PathLimitChecker::getSafePathLength();
		const size_t MAX_SINGLE_COMPONENT = PathLimitChecker::getMaxComponentLength();
		const size_t MAX_ESTIMATED_INTERNAL_PATH = 120; // Keep this as estimate

		bool pathTooLong = false;
		bool componentTooLong = false;
		bool estimatedOverflow = false;

		// Check if archive path is already too long
		if (archivePathLength > MAX_TOTAL_PATH)
		{
			pathTooLong = true;
		}

		// Check if archive filename itself is too long
		if (archiveFilename.length() > MAX_SINGLE_COMPONENT)
		{
			componentTooLong = true;
		}

		// Estimate if internal paths might cause overflow
		size_t estimatedMaxPath = basePathLength + archiveFilename.length() + MAX_ESTIMATED_INTERNAL_PATH;
		if (estimatedMaxPath > MAX_TOTAL_PATH)
		{
			estimatedOverflow = true;
		}

		if (pathTooLong || componentTooLong || estimatedOverflow)
		{
			std::wstring message = L"ARCHIVE SKIPPED - PATH LENGTH ISSUE:\n\n";
			message += L"Archive: " + UnicodeUtils::stringToWstring(archiveFilename) + L"\n\n";
			message += L"System Path Limit: " + std::to_wstring(PathLimitChecker::getMaxPathLength()) + L" chars\n";
			message += L"Long Path Support: " + std::wstring(PathLimitChecker::getMaxPathLength() > 260 ? L"ENABLED" : L"DISABLED") + L"\n\n";

			if (pathTooLong)
			{
				message += L"• Archive path too long: " + std::to_wstring(archivePathLength) +
					L" chars (max " + std::to_wstring(MAX_TOTAL_PATH) + L")\n";
			}
			if (componentTooLong)
			{
				message += L"• Archive filename too long: " + std::to_wstring(archiveFilename.length()) +
					L" chars (max " + std::to_wstring(MAX_SINGLE_COMPONENT) + L")\n";
			}
			if (estimatedOverflow)
			{
				message += L"• Estimated total path would exceed limit\n";
			}

			LockedMessageBox::showError(message, L"Archive Skipped - Path Too Long");
			return false;
		}

		return true;
	}

	bool extractAndCacheImageInternal(int targetIndex) {
		try
		{
			if (!archive || targetIndex < 0 || targetIndex >= imageEntries.size())
			{
				return false;
			}

			// Ensure cache is properly sized
			if (cachedImages.size() < imageEntries.size())
			{
				cachedImages.resize(imageEntries.size());
			}

			// If already cached, return success
			if (!cachedImages[targetIndex].empty())
			{
				return true;
			}

			// Reopen the archive to reset position
			std::wstring wPath = UnicodeUtils::stringToWstring(archivePath);

			// Close current archive
			if (archive)
			{
				archive_read_free(archive);
				archive = nullptr;
			}

			// Reopen archive with error checking
			archive = archive_read_new();
			if (!archive)
			{
				isArchiveOpen = false;
				ErrorDisplayHelper::showError(ErrorDisplayHelper::ErrorType::CRITICAL,
					ErrorDisplayHelper::ErrorContext()
					.setArchive(archivePathW)
					.setOperation("Archive Reopen")
					.setDetails("Failed to create new archive object for extraction"));
				return false;
			}

			archive_read_support_filter_all(archive);
			archive_read_support_format_all(archive);
			archive_read_set_option(archive, NULL, "hdrcharset", "UTF-8");

			if (archive_read_open_filename_w(archive, wPath.c_str(), 10240) != ARCHIVE_OK)
			{
				std::string errorMsg = "Failed to reopen archive for extraction";
				if (archive_error_string(archive))
				{
					errorMsg += ": " + std::string(archive_error_string(archive));
				}
				ErrorDisplayHelper::showError(ErrorDisplayHelper::ErrorType::CRITICAL,
					ErrorDisplayHelper::ErrorContext()
					.setArchive(archivePathW)
					.setOperation("Archive Reopen")
					.setDetails(errorMsg));
				archive_read_free(archive);
				archive = nullptr;
				isArchiveOpen = false;
				return false;
			}

			struct archive_entry* entry;
			bool found = false;
			int imageCount = 0; // Count only image files
			std::vector<std::string> foundPaths; // Debug: track what we find

			// Extract by position - much simpler and more reliable
			while (archive_read_next_header(archive, &entry) == ARCHIVE_OK)
			{
				const char* pathname = archive_entry_pathname(entry);

				// Get entry info
				la_int64_t entry_type = archive_entry_filetype(entry);
				la_int64_t size = archive_entry_size(entry);

				// Skip directories
				if (entry_type == AE_IFDIR)
				{
					archive_read_data_skip(archive);
					continue;
				}

				// Skip non-regular files
				if (entry_type != AE_IFREG)
				{
					archive_read_data_skip(archive);
					continue;
				}

				// Handle pathname
				std::string currentPath = pathname ? std::string(pathname) : ("unknown_" + std::to_string(imageCount));
				std::replace(currentPath.begin(), currentPath.end(), '\\', '/');

				// Check if it's an image file
				std::filesystem::path filePath(currentPath);
				std::string extension = filePath.extension().string();

				if (IsImgExtValid(extension) && size > 0)
				{
					foundPaths.push_back(currentPath); // Debug tracking

					// Is this our target image? (by position in image sequence)
					if (imageCount == targetIndex)
					{
						// This is our target image - extract it
						if (size > 500 * 1024 * 1024)
						{
							ErrorDisplayHelper::showError(ErrorDisplayHelper::ErrorType::MEMORY,
								ErrorDisplayHelper::ErrorContext()
								.setArchive(archivePathW)
								.setOperation("Image Too Large")
								.setMemorySize(static_cast<size_t>(size)));
							break;
						}

						try
						{
							cachedImages[targetIndex] = safeAllocateVector(static_cast<size_t>(size));

							la_ssize_t bytesRead = archive_read_data(archive,
								cachedImages[targetIndex].data(), cachedImages[targetIndex].size());

							if (bytesRead == size)
							{
								found = true;

								// Success message with debug info
								//std::wstring successMsg = L"EXTRACTION SUCCESS!\n\n";
								//successMsg += L"Target index: " + std::to_wstring(targetIndex) + L"\n";
								//successMsg += L"Image count: " + std::to_wstring(imageCount) + L"\n";
								//successMsg += L"Path found: " + UnicodeUtils::stringToWstring(currentPath) + L"\n";
								//successMsg += L"Size: " + std::to_wstring(size) + L" bytes\n";
								//successMsg += L"Bytes read: " + std::to_wstring(bytesRead);

								//MessageBoxW(NULL, successMsg.c_str(), L"Extraction Success", MB_OK | MB_ICONINFORMATION);

							}
							else if (bytesRead < 0)
							{
								std::string error = "Archive read error for: " + currentPath;
								if (archive_error_string(archive))
								{
									error += " - " + std::string(archive_error_string(archive));
								}
								ErrorDisplayHelper::showError(ErrorDisplayHelper::ErrorType::CRITICAL,
									ErrorDisplayHelper::ErrorContext()
									.setArchive(archivePathW)
									.setOperation("Archive Read Error")
									.setDetails(error));
								cachedImages[targetIndex].clear();
							}
							else
							{
								std::string error = "Partial read for: " + currentPath +
									". Expected: " + std::to_string(size) +
									", Got: " + std::to_string(bytesRead);
								ErrorDisplayHelper::showError(ErrorDisplayHelper::ErrorType::CRITICAL,
									ErrorDisplayHelper::ErrorContext()
									.setArchive(archivePathW)
									.setOperation("Partial Read")
									.setDetails(error));
								cachedImages[targetIndex].clear();
							}
						} catch (const std::bad_alloc& e)
						{
							ErrorDisplayHelper::showError(ErrorDisplayHelper::ErrorType::MEMORY,
								ErrorDisplayHelper::ErrorContext()
								.setArchive(archivePathW)
								.setOperation("Allocation Failed")
								.setMemorySize(static_cast<size_t>(size)));
							cachedImages[targetIndex].clear();
							found = false;
						}
						break; // Found our target, stop here
					}

					imageCount++; // Count this as an image
				}

				// Skip the data for this entry
				archive_read_data_skip(archive);
			}

			if (!found)
			{
				// Show debug info about what was found
				std::wstring message = L"EXTRACTION FAILED - DEBUG INFO:\n\n";
				message += L"Target index: " + std::to_wstring(targetIndex) + L"\n";
				message += L"Total images found during extraction: " + std::to_wstring(imageCount) + L"\n";
				message += L"Target path from loadImageEntries: " + UnicodeUtils::stringToWstring(imageEntries[targetIndex].name) + L"\n\n";

				message += L"Images found during extraction:\n";
				for (int i = 0; i < std::min(5, (int)foundPaths.size()); i++)
				{
					message += L"[" + std::to_wstring(i) + L"] " + UnicodeUtils::stringToWstring(foundPaths[i]) + L"\n";
				}
				if (foundPaths.size() > 5)
				{
					message += L"... and " + std::to_wstring(foundPaths.size() - 5) + L" more\n";
				}

				LockedMessageBox::showError(message, L"Extraction Debug");
			}

			return found;

		} catch (const std::exception& e)
		{
			ErrorDisplayHelper::showError(ErrorDisplayHelper::ErrorType::CRITICAL,
				ErrorDisplayHelper::ErrorContext()
				.setArchive(archivePathW)
				.setOperation("extractAndCacheImageInternal Exception")
				.setDetails(e.what()));
			return false;
		} catch (...)
		{
			ErrorDisplayHelper::showError(ErrorDisplayHelper::ErrorType::CRITICAL,
				ErrorDisplayHelper::ErrorContext()
				.setArchive(archivePathW)
				.setOperation("extractAndCacheImageInternal")
				.setDetails("Unknown exception occurred"));
			return false;
		}
	}
};

class ImageLoadingDispatcher {
public:
	struct LoadContext {
		bool isArchive;
		ArchiveHandler* archiveHandler;
		const std::vector<std::wstring>* currentImages;
		int imageIndex;

		LoadContext(bool archive, ArchiveHandler* handler,
			const std::vector<std::wstring>* images, int index)
			: isArchive(archive), archiveHandler(handler),
			currentImages(images), imageIndex(index) { }
	};

	static ImageLoader::LoadResult loadImageAtIndex(const LoadContext& context) {
		if (!context.currentImages || context.imageIndex < 0 ||
			context.imageIndex >= context.currentImages->size())
		{
			return ImageLoader::LoadResult("Invalid image index");
		}

		if (context.isArchive && context.archiveHandler)
		{
			return loadFromArchive(context);
		}
		else
		{
			return loadFromFile(context);
		}
	}

	static sf::Vector2u getImageDimensionsAtIndex(const LoadContext& context) {
		ImageLoader::LoadResult result = loadImageAtIndex(context);
		if (result.success)
		{
			return result.image.getSize();
		}
		return sf::Vector2u(0, 0);
	}

private:
	static ImageLoader::LoadResult loadFromArchive(const LoadContext& context) {
		std::vector<uint8_t> rawData;
		if (context.archiveHandler->extractImageToMemory(context.imageIndex, rawData))
		{
			std::string filename = getFilenameFromArchivePath((*context.currentImages)[context.imageIndex]);
			return ImageLoader::loadImageFromMemory(rawData, filename);
		}
		return ImageLoader::LoadResult("Failed to extract from archive");
	}

	static ImageLoader::LoadResult loadFromFile(const LoadContext& context) {
		return ImageLoader::loadImage((*context.currentImages)[context.imageIndex]);
	}

	static std::string getFilenameFromArchivePath(const std::wstring& archivePath) {
		std::string filename = UnicodeUtils::wstringToString(archivePath);
		size_t hashPos = filename.find('#');
		if (hashPos != std::string::npos)
		{
			filename = filename.substr(hashPos + 1);
		}
		std::filesystem::path p(filename);
		return p.filename().string();
	}
};

struct FoldersIdent {
	std::wstring dir;
	bool isArchieve;

	bool operator<(const FoldersIdent& other) const {
		return dir < other.dir;  // std::wstring already has operator<
	}
};

class ConfigManager {
private:
	std::wstring configFilePath;
	std::map<std::string, std::string> configData;

public:
	ConfigManager(const std::wstring& configPath = L"") : configFilePath() , configData() {

		if (configPath.empty())
		{
			// Default to executable directory + "config.ini"
			wchar_t exePath[MAX_PATH];
			GetModuleFileNameW(NULL, exePath, MAX_PATH);
			std::filesystem::path execDir = std::filesystem::path(exePath).parent_path();
			configFilePath = (execDir / "manga_reader_config.ini").wstring();
		}
		else
		{
			configFilePath = configPath;
		}
		loadConfig();
	}

	~ConfigManager() {
		saveConfig();
	}

	// Load configuration from INI file
	bool loadConfig() {
		configData.clear();

		try
		{
			std::ifstream file(configFilePath);
			if (!file.is_open())
			{
				// File doesn't exist yet, that's ok
				return true;
			}

			std::string line;
			std::string currentSection = "";

			while (std::getline(file, line))
			{
				line = UnicodeUtils::trim(line);

				// Skip empty lines and comments
				if (line.empty() || line[0] == ';' || line[0] == '#')
				{
					continue;
				}

				// Check for section headers [section]
				if (line[0] == '[' && line.back() == ']')
				{
					currentSection = line.substr(1, line.length() - 2);
					continue;
				}

				// Parse key=value pairs
				size_t equalPos = line.find('=');
				if (equalPos != std::string::npos)
				{
					std::string key = UnicodeUtils::trim(line.substr(0, equalPos));
					std::string value = UnicodeUtils::trim(line.substr(equalPos + 1));

					// Remove quotes if present
					if (value.length() >= 2 && value[0] == '"' && value.back() == '"')
					{
						value = value.substr(1, value.length() - 2);
					}

					// Store with section prefix if we have one
					std::string fullKey = currentSection.empty() ? key : currentSection + "." + key;
					configData[fullKey] = value;
				}
			}

			file.close();
			return true;

		} catch (const std::exception& e)
		{
			return false;
		}
	}

	// Save configuration to INI file
	bool saveConfig() {
		try
		{
			std::ofstream file(configFilePath);
			if (!file.is_open())
			{
				return false;
			}

			file << "; Manga Reader Configuration File\n";
			file << "; Auto-generated - modify with care\n\n";

			// Group by sections
			std::map<std::string, std::map<std::string, std::string>> sections;

			for (const auto& pair : configData)
			{
				size_t dotPos = pair.first.find('.');
				if (dotPos != std::string::npos)
				{
					std::string section = pair.first.substr(0, dotPos);
					std::string key = pair.first.substr(dotPos + 1);
					sections[section][key] = pair.second;
				}
				else
				{
					sections[""][pair.first] = pair.second;
				}
			}

			// Write sections
			for (const auto& section : sections)
			{
				if (!section.first.empty())
				{
					file << "[" << section.first << "]\n";
				}

				for (const auto& keyValue : section.second)
				{
					file << keyValue.first << "=" << keyValue.second << "\n";
				}

				file << "\n";
			}

			file.close();
			return true;

		} catch (const std::exception& e)
		{
			return false;
		}
	}

	// Get string value
	std::string getString(const std::string& key, const std::string& defaultValue = "") {
		auto it = configData.find(key);
		return (it != configData.end()) ? it->second : defaultValue;
	}

	// Get wide string value
	std::wstring getWString(const std::string& key, const std::wstring& defaultValue = L"") {
		std::string value = getString(key);
		return value.empty() ? defaultValue : UnicodeUtils::stringToWstring(value);
	}

	// Get integer value
	int getInt(const std::string& key, int defaultValue = 0) {
		std::string value = getString(key);
		if (value.empty()) return defaultValue;

		try
		{
			return std::stoi(value);
		} catch (...)
		{
			return defaultValue;
		}
	}

	// Get boolean value
	bool getBool(const std::string& key, bool defaultValue = false) {
		std::string value = getString(key);
		if (value.empty()) return defaultValue;

		std::transform(value.begin(), value.end(), value.begin(), ::tolower);
		return (value == "true" || value == "1" || value == "yes" || value == "on");
	}

	// Get float value
	float getFloat(const std::string& key, float defaultValue = 0.0f) {
		std::string value = getString(key);
		if (value.empty()) return defaultValue;

		try
		{
			return std::stof(value);
		} catch (...)
		{
			return defaultValue;
		}
	}

	// Set string value
	void setString(const std::string& key, const std::string& value) {
		configData[key] = value;
	}

	// Set wide string value
	void setWString(const std::string& key, const std::wstring& value) {
		configData[key] = UnicodeUtils::wstringToString(value);
	}

	// Set integer value
	void setInt(const std::string& key, int value) {
		configData[key] = std::to_string(value);
	}

	// Set boolean value
	void setBool(const std::string& key, bool value) {
		configData[key] = value ? "true" : "false";
	}

	// Set float value
	void setFloat(const std::string& key, float value) {
		configData[key] = std::to_string(value);
	}

	// Check if key exists
	bool hasKey(const std::string& key) {
		return configData.find(key) != configData.end();
	}

	// Remove a key
	void removeKey(const std::string& key) {
		configData.erase(key);
	}

	// Get config file path
	std::wstring getConfigFilePath() const {
		return configFilePath;
	}

	// Force save config now
	void forceSave() {
		saveConfig();
	}
};

enum class ButtonID {
	INFO_BUTTON,
	PREVIOUS_FOLDER,
	NEXT_FOLDER,
	SETTINGS_BUTTON,
	HELP_BUTTON,
	COUNT
};

enum class SessionChoice {
	RESTORE_SESSION,
	NEW_SESSION,
	CANCELLED
};

class UIButton {
public:
	struct ButtonConfig {
		std::string text = "?";
		sf::Color backgroundColor = sf::Color(100, 100, 100, 200);
		sf::Color textColor = sf::Color::White;
		sf::Color outlineColor = sf::Color::White;
		sf::Color disabledBgColor = sf::Color(60, 60, 60, 150);
		sf::Color disabledTextColor = sf::Color(150, 150, 150);
		bool hasCircularBg = true;
		unsigned int fontSize = 18;
	};

private:
	ButtonID buttonID;
	sf::RectangleShape button;
	sf::CircleShape circularBg;
	sf_text_wrapper buttonText;
	sf::Vector2f position;
	float size;
	bool isEnabled;
	bool hasToggleState;
	bool isToggled;
	ButtonConfig config;

public:

	UIButton() : buttonID(ButtonID::INFO_BUTTON), button(), circularBg(), buttonText(),
		position(0, 0), size(30.0f), isEnabled(true), hasToggleState(false),
		isToggled(false), config() { }

	// Delete copy constructor and copy assignment
	UIButton(const UIButton&) = delete;
	UIButton& operator=(const UIButton&) = delete;

	// Add move constructor and move assignment
	UIButton(UIButton&&) = default;
	UIButton& operator=(UIButton&&) = default;

	void initialize(const sf::Font& font, ButtonID id, float x, float y,
		const ButtonConfig& buttonConfig, float buttonSize = 30.0f) {
		buttonID = id;
		config = buttonConfig;
		size = buttonSize;
		position = sf::Vector2f(x, y);

		// Setup button background
		button.setSize(sf::Vector2f(size, size));
		button.setPosition(position);
		button.setOutlineThickness(1);

		// Setup circular background if needed
		if (config.hasCircularBg)
		{
			circularBg.setRadius(size / 2 - 3);
			circularBg.setPosition(sf::Vector2f(x + 3, y + 3));
		}

		// Setup text
		buttonText.initialize(font, config.fontSize);
		buttonText.get()->setString(config.text);
		buttonText.get()->setStyle(sf::Text::Bold);

		// Set initial appearance
		updateAppearance();
		centerText();
	}

	void updatePosition(float x, float y) {
		position = sf::Vector2f(x, y);
		button.setPosition(position);

		if (config.hasCircularBg)
		{
			circularBg.setPosition(sf::Vector2f(x + 3, y + 3));
		}

		centerText();
	}

	void setEnabled(bool enabled) {
		isEnabled = enabled;
		updateAppearance();
	}

	void setToggleState(bool canToggle, bool initialState = false) {
		hasToggleState = canToggle;
		isToggled = initialState;
		updateAppearance();
	}

	void toggle() {
		if (hasToggleState)
		{
			isToggled = !isToggled;
			updateAppearance();
		}
	}

	bool isClicked(sf::Vector2f mousePos, float expandBy = 5.0f) const {
		if (!isEnabled) return false;

		sf::FloatRect bounds = button.getGlobalBounds();
		bounds.position.x -= expandBy;
		bounds.position.y -= expandBy;
		bounds.size.x += expandBy * 2;
		bounds.size.y += expandBy * 2;

		return bounds.contains(mousePos);
	}

	// Getters
	ButtonID getID() const { return buttonID; }
	bool getIsEnabled() const { return isEnabled; }
	bool getIsToggled() const { return isToggled; }
	sf::Vector2f getPosition() const { return position; }
	float getSize() const { return size; }
	sf::FloatRect getBounds() const { return button.getGlobalBounds(); }

	void draw(sf::RenderWindow& window) {
		window.draw(button);
		if (config.hasCircularBg)
		{
			window.draw(circularBg);
		}
		window.draw(*buttonText.get());
	}

private:
	void updateAppearance() {
		if (isEnabled)
		{
			if (hasToggleState && isToggled)
			{
				// Toggled state (brighter/different color)
				button.setFillColor(sf::Color(100, 160, 210, 220));
				button.setOutlineColor(sf::Color::Cyan);
				if (config.hasCircularBg)
				{
					circularBg.setFillColor(sf::Color(200, 230, 255, 180));
				}
				buttonText.get()->setFillColor(sf::Color(70, 130, 180));
			}
			else
			{
				// Normal enabled state
				button.setFillColor(config.backgroundColor);
				button.setOutlineColor(config.outlineColor);
				if (config.hasCircularBg)
				{
					circularBg.setFillColor(sf::Color(255, 255, 255, 180));
				}
				buttonText.get()->setFillColor(config.textColor);
			}
		}
		else
		{
			// Disabled state
			button.setFillColor(config.disabledBgColor);
			button.setOutlineColor(sf::Color(120, 120, 120));
			if (config.hasCircularBg)
			{
				circularBg.setFillColor(sf::Color(200, 200, 200, 100));
			}
			buttonText.get()->setFillColor(config.disabledTextColor);
		}
	}

	void centerText() {
		sf::FloatRect textBounds = buttonText.get()->getLocalBounds();
		buttonText.get()->setPosition(sf::Vector2f(
			position.x + (size - textBounds.size.x) / 2 - textBounds.position.x,
			position.y + (size - textBounds.size.y) / 2 - textBounds.position.y
		));
	}
};

class UIButtonManager {
private:
	std::vector<std::unique_ptr<UIButton>> buttons;
	std::map<ButtonID, size_t> buttonIndexMap;
	mutable std::shared_mutex buttonMutex;
public:
	UIButtonManager() : buttons(), buttonIndexMap(), buttonMutex() { buttons.reserve(10); }

	UIButton* getButtonInternal(ButtonID id) {
		auto it = buttonIndexMap.find(id);
		return (it != buttonIndexMap.end()) ? buttons[it->second].get() : nullptr;
	}

	void addButton(const sf::Font& font, ButtonID id, float x, float y,
		const UIButton::ButtonConfig& config, float size = 30.0f) {
		std::unique_lock<std::shared_mutex> lock(buttonMutex);
		size_t index = buttons.size();
		auto button = std::make_unique<UIButton>();
		button->initialize(font, id, x, y, config, size);
		buttons.push_back(std::move(button));
		buttonIndexMap[id] = index;
	}

	template<typename Func>
	auto withButton(ButtonID id, Func&& func) -> decltype(func(std::declval<UIButton*>())) {
		std::shared_lock<std::shared_mutex> lock(buttonMutex);  // Shared lock for reading
		UIButton* btn = getButtonInternal(id);
		return func(btn);
	}

	// Get button by ID
	UIButton* getButton(ButtonID id) {
		std::shared_lock<std::shared_mutex> lock(buttonMutex);  // NEW: Shared lock
		return getButtonInternal(id);
	}

	// Update all button positions (useful for window resize)
	void updateAllPositions(const std::function<sf::Vector2f(ButtonID)>& positionCalculator) {
		std::unique_lock<std::shared_mutex> lock(buttonMutex);  // NEW: Exclusive lock
		for (auto& button : buttons)
		{
			sf::Vector2f newPos = positionCalculator(button->getID());
			button->updatePosition(newPos.x, newPos.y);
		}
	}

	// Check which button was clicked (returns ButtonID::COUNT if none)
	ButtonID checkButtonClick(sf::Vector2f mousePos, float expandBy = 5.0f) {
		std::shared_lock<std::shared_mutex> lock(buttonMutex);  // NEW: Shared lock
		for (const auto& button : buttons)
		{
			if (button->isClicked(mousePos, expandBy))
			{
				return button->getID();
			}
		}
		return ButtonID::COUNT;
	}

	// Batch operations
	void enableButton(ButtonID id, bool enabled) {
		withButton(id, [enabled](UIButton* btn) {
			if (btn) btn->setEnabled(enabled);
			return true;
			});
	}

	void toggleButton(ButtonID id) {
		withButton(id, [](UIButton* btn) {
			if (btn) btn->toggle();
			return true;
			});
	}

	bool isButtonToggled(ButtonID id) {
		return withButton(id, [](UIButton* btn) {
			return btn ? btn->getIsToggled() : false;
			});
	}

	// Render all buttons
	void drawAll(sf::RenderWindow& window) {
		std::shared_lock<std::shared_mutex> lock(buttonMutex);
		for (auto& button : buttons)
		{
			button->draw(window);
		}
	}

	// Get button count
	size_t getButtonCount() const {
		std::shared_lock<std::shared_mutex> lock(buttonMutex);
		return buttons.size();
	}

	// Clear all buttons
	void clear() {
		std::unique_lock<std::shared_mutex> lock(buttonMutex);
		buttons.clear();
		buttonIndexMap.clear();
	}
};

static constexpr const char* CONFIG_SECTION = "Settings";
static constexpr const char* CONFIG_LAST_FOLDER = "Settings.lastMangaFolder";
static constexpr const char* CONFIG_LAST_FOLDER_INDEX = "Settings.lastFolderIndex";
static constexpr const char* CONFIG_LAST_IMAGE_INDEX = "Settings.lastImageIndex";
static constexpr const char* CONFIG_WINDOW_WIDTH = "Settings.windowWidth";
static constexpr const char* CONFIG_WINDOW_HEIGHT = "Settings.windowHeight";
static constexpr const char* CONFIG_WINDOW_MAXIMIZED = "Settings.windowMaximized";
static constexpr const char* CONFIG_WINDOW_FULLSCREEN = "Settings.windowFullscreen";
static constexpr const char* CONFIG_USE_SMOOTHING = "Settings.useSmoothing";
static constexpr const char* CONFIG_ASK_SESSION_RESTORE = "Settings.askSessionRestore";
static constexpr const char* CONFIG_LAST_SESSION_EXISTS = "Settings.lastSessionExists";
static constexpr const char* CONFIG_SHOW_SESSION_SUCCESS = "Settings.showSessionSuccessDialog";


struct CommandLineOptions {
	bool enableLongPaths = false;
	bool showPathInfo = false;
	bool verbose = false;
	std::string configFile = "";
	std::string mangaFolder = "";
};

class MangaReader {
private:
	CommandLineOptions cmdOptions;

	sf::RenderWindow window;

	sf::Texture originalTexture;      // Store original high-res texture
	sf::Texture scaledTexture;        // Store scaled texture for display

	sf_Sprite_wrapper currentSprite;
	sf_font_wrapper font;
	sf_text_wrapper statusText;
	sf_text_wrapper helpText;
	sf_text_wrapper detailedInfoText;

	std::vector<FoldersIdent> folders;
	std::vector<std::wstring> currentImages;
	int currentFolderIndex;
	int currentImageIndex;

	float scrollOffset;
	float zoomLevel;
	sf::Vector2f imagePosition;

	// Scaling support
	bool useSmoothing;
	float lastZoomLevel;              // Track zoom changes for rescaling
	sf::Vector2u lastWindowSize;     // Track window size changes

	bool showUI;
	std::wstring rootMangaPath;

	// Archive support
	ArchiveHandler archiveHandler;
	bool isCurrentlyInArchive;
	std::wstring currentArchivePath;

	struct LoadedImageData {
		sf::Image image;
		std::string filename;
		size_t fileSize;
		bool isLoaded;

		LoadedImageData() : fileSize(0), isLoaded(false) { }
	};

	std::vector<LoadedImageData> loadedImages;
	std::mutex loadingMutex;
	std::atomic<bool> isLoadingFolder;
	std::atomic<int> loadingProgress;
	std::future<void> folderLoadingFuture;

	// Progress display
	sf_text_wrapper loadingText;

	// Enhanced zoom and positioning management
	float savedZoomLevel;          // Remember zoom level across images in same folder
	sf::Vector2f savedImageOffset; // Remember manual positioning offset
	bool hasCustomZoom;            // Track if user has set custom zoom
	bool hasCustomPosition;        // Track if user has manually positioned image
	sf::View currentView;          // Manage view properly

	ImageSizeMismatchHandler sizeMismatchHandler;
	NavigationLockManager navLock;

	UIButtonManager buttonManager;
	std::unique_ptr<ConfigManager> config;

	bool showHelpText;

	bool wasMaximizedOnStart;
	bool isCurrentlyMaximized;
	bool isCurrentlyFullscreen;
	RECT windowedRect;
	LONG windowedStyle;
	LONG windowedExStyle;

public: //constructor and destructor

	explicit MangaReader(const CommandLineOptions& options): cmdOptions(options)
		 , window()
		 , originalTexture()
		 , scaledTexture()
		 , currentSprite()
		 , font()
		 , statusText()
		 , helpText()
		 , detailedInfoText()
		 , folders()
		 , currentImages()
		 , currentFolderIndex(0)
		 , currentImageIndex(0)
		 , scrollOffset(0.0f)
		 , zoomLevel(1.0f)
		 , imagePosition()
		 , useSmoothing(true)
		 , lastZoomLevel()
		 , lastWindowSize()
		 , showUI(true)
		 , rootMangaPath()
		 , archiveHandler()
		 , isCurrentlyInArchive()
		 , currentArchivePath()
		 , loadedImages()
		 , loadingMutex()
		 , isLoadingFolder(false)
		 , loadingProgress(0)
		 , folderLoadingFuture()
		 , loadingText()
		 , savedZoomLevel(1.0f)
		 , savedImageOffset()
		 , hasCustomZoom()
		 , hasCustomPosition()
		 , currentView()
		 , sizeMismatchHandler()
		 , navLock()
		 , buttonManager()
		 , config()
		 , showHelpText(true)
		 , wasMaximizedOnStart(false)
		 , isCurrentlyMaximized(false)
		 , isCurrentlyFullscreen(false)
		 , windowedRect({ 0, 0, 0, 0 })
		 , windowedStyle(0)
		 , windowedExStyle(0)
	{
		// Step 1: Create config FIRST (before any validation or window creation)
		if (!cmdOptions.configFile.empty())
		{
			config = std::make_unique<ConfigManager>(
				UnicodeUtils::stringToWstring(cmdOptions.configFile)
			);
		}
		else
		{
			config = std::make_unique<ConfigManager>();
		}

		// Step 2: Validate command line paths (now we can show errors properly)
		if (!validateCommandLinePaths())
		{
			// Create a minimal window for error display, then exit
			createMinimalWindow();
			return;
		}

		// Step 3: Initialize window (now config exists)
		initializeWindow();

		// Step 4: Setup UI components
		setupUI();

		loadingText.initialize(*font.get(), 18u);
		loadingText.get()->setFillColor(sf::Color::White);

		// Step 5: Initialize with command line manga folder if provided and valid
		if (!cmdOptions.mangaFolder.empty())
		{
			rootMangaPath = UnicodeUtils::stringToWstring(cmdOptions.mangaFolder);
			initializeWithFolder();
		}
		else
		{
			initializeConfig();
		}
	}

	void initializeWithFolder() {
		if (rootMangaPath.empty())
		{
			// Fallback to normal initialization if path is empty
			initializeConfig();
			return;
		}

		try
		{
			if (!std::filesystem::exists(rootMangaPath))
			{
				std::wstring errorMsg = L"Manga folder does not exist: " + rootMangaPath +
					L"\n\nFalling back to folder selection dialog.";
				LockedMessageBox::showWarning(errorMsg, L"Folder Not Found");
				initializeConfig();
				return;
			}

			if (!std::filesystem::is_directory(rootMangaPath))
			{
				std::wstring errorMsg = L"Path is not a directory: " + rootMangaPath +
					L"\n\nFalling back to folder selection dialog.";
				LockedMessageBox::showWarning(errorMsg, L"Invalid Directory");
				initializeConfig();
				return;
			}

			loadFolders(rootMangaPath);
			updateNavigationButtons();

			if (!folders.empty())
			{
				currentFolderIndex = 0;
				loadImagesFromFolder(folders[currentFolderIndex]);

				if (!currentImages.empty())
				{
					currentImageIndex = 0;
					if (loadCurrentImage())
					{
						updateWindowTitle();
						return;
					}
				}
			}

			// If we reach here, the folder exists but has no valid content
			std::wstring errorMsg = L"No manga content found in: " + rootMangaPath +
				L"\n\nFalling back to folder selection dialog.";
			LockedMessageBox::showWarning(errorMsg, L"No Content Found");
			initializeConfig();

		} catch (const std::filesystem::filesystem_error& e)
		{
			std::wstring errorMsg = L"Filesystem error accessing: " + rootMangaPath +
				L"\nError: " + UnicodeUtils::stringToWstring(e.what()) +
				L"\n\nFalling back to folder selection dialog.";
			LockedMessageBox::showWarning(errorMsg, L"Filesystem Error");
			initializeConfig();
		} catch (const std::exception& e)
		{
			std::wstring errorMsg = L"Error processing manga folder: " + rootMangaPath +
				L"\nError: " + UnicodeUtils::stringToWstring(e.what()) +
				L"\n\nFalling back to folder selection dialog.";
			LockedMessageBox::showWarning(errorMsg, L"Processing Error");
			initializeConfig();
		}
	}

	~MangaReader() {
		saveCurrentSession();

		//Cleanup COM
		CoUninitialize();
	}

	bool validateCommandLinePaths() {
		bool isValid = true;

		// Validate config file path
		if (!cmdOptions.configFile.empty())
		{
			try
			{
				if (!std::filesystem::exists(cmdOptions.configFile))
				{
					std::wstring errorMsg = L"Configuration file not found:\n" +
						UnicodeUtils::stringToWstring(cmdOptions.configFile) +
						L"\n\nApplication will use default configuration.";
					LockedMessageBox::showWarning(errorMsg, L"Config File Not Found");
					// Don't fail for missing config file, just warn
				}
				else if (!std::filesystem::is_regular_file(cmdOptions.configFile))
				{
					std::wstring errorMsg = L"Configuration path is not a file:\n" +
						UnicodeUtils::stringToWstring(cmdOptions.configFile) +
						L"\n\nApplication will use default configuration.";
					LockedMessageBox::showWarning(errorMsg, L"Invalid Config Path");
					// Don't fail for invalid config file, just warn
				}
			} catch (const std::filesystem::filesystem_error& e)
			{
				std::wstring errorMsg = L"Error accessing configuration file:\n" +
					UnicodeUtils::stringToWstring(cmdOptions.configFile) +
					L"\nError: " + UnicodeUtils::stringToWstring(e.what()) +
					L"\n\nApplication will use default configuration.";
				LockedMessageBox::showWarning(errorMsg, L"Config Access Error");
				// Don't fail for config access errors, just warn
			}
		}

		// Validate manga folder path
		if (!cmdOptions.mangaFolder.empty())
		{
			try
			{
				std::filesystem::path mangaPath(cmdOptions.mangaFolder);

				if (!std::filesystem::exists(mangaPath))
				{
					std::wstring errorMsg = L"Manga folder not found:\n" +
						UnicodeUtils::stringToWstring(cmdOptions.mangaFolder) +
						L"\n\nApplication will start with folder selection dialog.";
					LockedMessageBox::showWarning(errorMsg, L"Manga Folder Not Found");
					// Clear the invalid path but don't fail
					cmdOptions.mangaFolder.clear();
				}
				else if (!std::filesystem::is_directory(mangaPath))
				{
					std::wstring errorMsg = L"Manga path is not a directory:\n" +
						UnicodeUtils::stringToWstring(cmdOptions.mangaFolder) +
						L"\n\nApplication will start with folder selection dialog.";
					LockedMessageBox::showWarning(errorMsg, L"Invalid Manga Path");
					// Clear the invalid path but don't fail
					cmdOptions.mangaFolder.clear();
				}
			} catch (const std::filesystem::filesystem_error& e)
			{
				std::wstring errorMsg = L"Error accessing manga folder:\n" +
					UnicodeUtils::stringToWstring(cmdOptions.mangaFolder) +
					L"\nError: " + UnicodeUtils::stringToWstring(e.what()) +
					L"\n\nApplication will start with folder selection dialog.";
				LockedMessageBox::showWarning(errorMsg, L"Manga Folder Access Error");
				// Clear the invalid path but don't fail
				cmdOptions.mangaFolder.clear();
			}
		}

		return isValid; // Always return true now - we handle errors gracefully
	}

	void createMinimalWindow() {
		// Create minimal window just for error display
		window.create(sf::VideoMode(sf::Vector2u(800, 600)), "Simple Manga Reader - Error");

		HWND hwnd = window.getNativeHandle();
		LockedMessageBox::setMainWindow(hwnd);

		// Initialize COM for error dialogs
		CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
	}

	void storeWindowedProperties() {
		HWND hwnd = window.getNativeHandle();
		if (!hwnd) return;

		// Store current window rectangle and styles
		GetWindowRect(hwnd, &windowedRect);
		windowedStyle = GetWindowLongW(hwnd, GWL_STYLE);
		windowedExStyle = GetWindowLongW(hwnd, GWL_EXSTYLE);
	}

	bool getIsFullscreen() const {
		return isCurrentlyFullscreen;
	}

	void enterFullscreen() {
		HWND hwnd = window.getNativeHandle();
		if (!hwnd || isCurrentlyFullscreen) return;

		// Store current windowed state before going fullscreen
		if (!isCurrentlyFullscreen)
		{
			storeWindowedProperties();
		}

		// Get monitor info for the window
		HMONITOR monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
		MONITORINFO monitorInfo = { sizeof(monitorInfo) };
		GetMonitorInfo(monitor, &monitorInfo);

		// Remove window decorations
		LONG newStyle = windowedStyle;
		newStyle &= ~(WS_CAPTION | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_SYSMENU);
		SetWindowLongW(hwnd, GWL_STYLE, newStyle);

		LONG newExStyle = windowedExStyle;
		newExStyle &= ~(WS_EX_DLGMODALFRAME | WS_EX_WINDOWEDGE | WS_EX_CLIENTEDGE | WS_EX_STATICEDGE);
		SetWindowLongW(hwnd, GWL_EXSTYLE, newExStyle);

		// Set window to cover entire monitor
		SetWindowPos(hwnd, HWND_TOP,
			monitorInfo.rcMonitor.left,
			monitorInfo.rcMonitor.top,
			monitorInfo.rcMonitor.right - monitorInfo.rcMonitor.left,
			monitorInfo.rcMonitor.bottom - monitorInfo.rcMonitor.top,
			SWP_NOOWNERZORDER | SWP_FRAMECHANGED);

		isCurrentlyFullscreen = true;
		isCurrentlyMaximized = false; // Can't be maximized in fullscreen
	}

	void exitFullscreen() {
		HWND hwnd = window.getNativeHandle();
		if (!hwnd || !isCurrentlyFullscreen) return;

		// Restore original window styles
		SetWindowLongW(hwnd, GWL_STYLE, windowedStyle);
		SetWindowLongW(hwnd, GWL_EXSTYLE, windowedExStyle);

		// Restore original window position and size
		SetWindowPos(hwnd, HWND_NOTOPMOST,
			windowedRect.left,
			windowedRect.top,
			windowedRect.right - windowedRect.left,
			windowedRect.bottom - windowedRect.top,
			SWP_NOOWNERZORDER | SWP_FRAMECHANGED);

		// Restore maximized state if it was maximized before
		if (wasMaximizedOnStart)
		{
			ShowWindow(hwnd, SW_MAXIMIZE);
			isCurrentlyMaximized = true;
		}

		isCurrentlyFullscreen = false;
	}

	void toggleFullscreen() {
		if (isCurrentlyFullscreen)
		{
			exitFullscreen();
		}
		else
		{
			enterFullscreen();
		}

		// Update view after fullscreen toggle
		sf::Vector2u newSize = window.getSize();
		handleWindowResize(newSize);
	}

	void initializeWindow() {

		int savedWidth = config->getInt(CONFIG_WINDOW_WIDTH, 1200);
		int savedHeight = config->getInt(CONFIG_WINDOW_HEIGHT, 800);
		bool savedMaximized = config->getBool(CONFIG_WINDOW_MAXIMIZED, false);
		bool savedFullscreen = config->getBool(CONFIG_WINDOW_FULLSCREEN, false);

		// Store initial states
		wasMaximizedOnStart = savedMaximized;
		isCurrentlyMaximized = savedMaximized;
		isCurrentlyFullscreen = false; // Always start windowed, then apply fullscreen

		// Create window with saved dimensions
		window.create(sf::VideoMode(sf::Vector2u(savedWidth, savedHeight)), "Simple Manga Reader");
		window.setFramerateLimit(60);

		// Get the native window handle
		HWND hwnd = window.getNativeHandle();
		LockedMessageBox::setMainWindow(hwnd);

		// Store initial windowed properties
		storeWindowedProperties();

		// Apply saved states
		if (savedFullscreen)
		{
			enterFullscreen();
		}
		else if (savedMaximized)
		{
			ShowWindow(hwnd, SW_MAXIMIZE);
			isCurrentlyMaximized = true;
		}

		// Initialize view based on current window size
		sf::Vector2u windowSize = window.getSize();
		currentView.setSize({ static_cast<float>(windowSize.x), static_cast<float>(windowSize.y) });
		currentView.setCenter({ static_cast<float>(windowSize.x) / 2.0f, static_cast<float>(windowSize.y) / 2.0f });
		window.setView(currentView);

		// Initialize COM for folder dialog
		CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
		SetConsoleOutputCP(CP_UTF8);
		SetConsoleCP(CP_UTF8);
	}

	void updateWindowTitle() {
		std::wstring title = L"Simple Manga Reader";

		if (!folders.empty() && currentFolderIndex >= 0 && currentFolderIndex < folders.size())
		{
			// Get current folder/archive name
			std::wstring currentPath = folders[currentFolderIndex].dir;
			std::wstring folderName;

			try
			{
				std::filesystem::path path(currentPath);
				folderName = path.filename().wstring();

				// If it's empty (root directory case), use parent directory name
				if (folderName.empty())
				{
					folderName = path.parent_path().filename().wstring();
				}
			} catch (...)
			{
				folderName = L"Unknown";
			}

			// Build title with folder info
			title += L" - " + folderName;

			// Add archive indicator if needed
			if (isCurrentlyInArchive)
			{
				title += L" [Archive]";
			}

			// Add progress info if images are loaded
			if (!currentImages.empty())
			{
				title += L" (" + std::to_wstring(currentImageIndex + 1) +
					L"/" + std::to_wstring(currentImages.size()) + L")";
			}

			// Add folder progress
			if (folders.size() > 1)
			{
				title += L" [" + std::to_wstring(currentFolderIndex + 1) +
					L"/" + std::to_wstring(folders.size()) + L"]";
			}
		}

		window.setTitle(title);
	}

	void initializeButtons() {
		UIButton::ButtonConfig infoConfig;
		infoConfig.text = "i";
		infoConfig.backgroundColor = sf::Color(70, 130, 180, 200);
		infoConfig.textColor = sf::Color(70, 130, 180);
		infoConfig.hasCircularBg = true;
		infoConfig.fontSize = 22;

		UIButton::ButtonConfig prevConfig;
		prevConfig.text = "<";
		prevConfig.backgroundColor = sf::Color(100, 100, 100, 200);
		prevConfig.textColor = sf::Color(100, 100, 100);
		prevConfig.hasCircularBg = true;
		prevConfig.fontSize = 20;

		UIButton::ButtonConfig nextConfig;
		nextConfig.text = ">";
		nextConfig.backgroundColor = sf::Color(100, 100, 100, 200);
		nextConfig.textColor = sf::Color(100, 100, 100);
		nextConfig.hasCircularBg = true;
		nextConfig.fontSize = 20;

		UIButton::ButtonConfig helpConfig;
		helpConfig.text = "H";
		helpConfig.backgroundColor = sf::Color(50, 150, 50, 200);
		helpConfig.textColor = sf::Color(50, 150, 50);
		helpConfig.hasCircularBg = true;
		helpConfig.fontSize = 18;

		UIButton::ButtonConfig settingsConfig;
		settingsConfig.text = "S";
		settingsConfig.backgroundColor = sf::Color(150, 100, 50, 200);
		settingsConfig.textColor = sf::Color(150, 100, 50);
		settingsConfig.hasCircularBg = true;
		settingsConfig.fontSize = 18;

		// Calculate initial positions
		float buttonY = 10.0f;
		float buttonSize = 30.0f;
		float spacing = 35.0f;
		float infoButtonX = static_cast<float>(window.getSize().x) - 50.0f;

		// Add buttons to manager
		buttonManager.addButton(*font.get(), ButtonID::INFO_BUTTON, infoButtonX, buttonY, infoConfig, buttonSize);
		buttonManager.addButton(*font.get(), ButtonID::NEXT_FOLDER, infoButtonX - spacing, buttonY, nextConfig, buttonSize);
		buttonManager.addButton(*font.get(), ButtonID::PREVIOUS_FOLDER, infoButtonX - (spacing * 2), buttonY, prevConfig, buttonSize);
		buttonManager.addButton(*font.get(), ButtonID::HELP_BUTTON, infoButtonX - (spacing * 3), buttonY, helpConfig, buttonSize);
		buttonManager.addButton(*font.get(), ButtonID::SETTINGS_BUTTON, infoButtonX - (spacing * 4), buttonY, settingsConfig, buttonSize);

		// Set up info button as toggleable
		buttonManager.getButton(ButtonID::INFO_BUTTON)->setToggleState(true, false);
		buttonManager.getButton(ButtonID::HELP_BUTTON)->setToggleState(true, true);

		// Update button states
		updateNavigationButtons();
	}

	void browseFolderOnStartup() {
		rootMangaPath = browseForFolder();
		if (rootMangaPath.empty())
		{
			LockedMessageBox::showError(L"No folder selected. Application will exit.", L"No Folder Selected");
			window.close();
			return;
		}

		loadFolders(rootMangaPath);
		if (folders.empty())
		{
			LockedMessageBox::showError(
				L"No manga folders or archives found in the selected directory.\n"
				L"Make sure the selected folder contains subfolders with images or archive files.",
				L"No Manga Found");
			window.close();
			return;
		}

		updateNavigationButtons();

		// Start with first folder and image
		currentFolderIndex = 0;
		currentImageIndex = 0;

		bool foundWorkingFolder = false;
		for (int i = 0; i < folders.size() && !foundWorkingFolder; i++)
		{
			currentFolderIndex = i;
			try
			{
				loadImagesFromFolder(folders[currentFolderIndex]);
				if (!currentImages.empty() && loadCurrentImage())
				{
					foundWorkingFolder = true;
					updateWindowTitle();
				}
			} catch (...)
			{
				continue;
			}
		}

		if (!foundWorkingFolder)
		{
			LockedMessageBox::showError(L"No working manga folders or archives found.", L"No Working Content");
			window.close();
		}
	}

	void saveCurrentSession() {
		if (!config) return;

		try
		{
			// Save current state
			config->setWString(CONFIG_LAST_FOLDER, rootMangaPath);
			config->setInt(CONFIG_LAST_FOLDER_INDEX, currentFolderIndex);
			config->setInt(CONFIG_LAST_IMAGE_INDEX, currentImageIndex);

			// Mark that we have session data
			config->setBool(CONFIG_LAST_SESSION_EXISTS, true);

			// Save window settings
			HWND hwnd = window.getNativeHandle();
			if (hwnd)
			{
				// Save fullscreen state
				config->setBool(CONFIG_WINDOW_FULLSCREEN, isCurrentlyFullscreen);

				if (!isCurrentlyFullscreen)
				{
					bool isMaximized = IsZoomed(hwnd) == TRUE;
					config->setBool(CONFIG_WINDOW_MAXIMIZED, isMaximized);

					if (!isMaximized)
					{
						sf::Vector2u windowSize = window.getSize();
						config->setInt(CONFIG_WINDOW_WIDTH, windowSize.x);
						config->setInt(CONFIG_WINDOW_HEIGHT, windowSize.y);
					}
				}
				else
				{
					config->setBool(CONFIG_WINDOW_MAXIMIZED, wasMaximizedOnStart);
					if (!wasMaximizedOnStart && windowedRect.right > windowedRect.left)
					{
						config->setInt(CONFIG_WINDOW_WIDTH, windowedRect.right - windowedRect.left);
						config->setInt(CONFIG_WINDOW_HEIGHT, windowedRect.bottom - windowedRect.top);
					}
				}
			}
			else
			{
				sf::Vector2u windowSize = window.getSize();
				config->setInt(CONFIG_WINDOW_WIDTH, windowSize.x);
				config->setInt(CONFIG_WINDOW_HEIGHT, windowSize.y);
				config->setBool(CONFIG_WINDOW_MAXIMIZED, isCurrentlyMaximized);
				config->setBool(CONFIG_WINDOW_FULLSCREEN, isCurrentlyFullscreen);
			}

			config->setBool(CONFIG_USE_SMOOTHING, useSmoothing);
			config->setBool("UI.infoButtonVisible", buttonManager.isButtonToggled(ButtonID::INFO_BUTTON));
			config->setBool("UI.helpButtonVisible", buttonManager.isButtonToggled(ButtonID::HELP_BUTTON));

			// Force save to disk
			config->forceSave();

		} catch (const std::exception& e)
		{
			// Save failed, but don't interrupt user experience
		}
	}

	void resetSessionRestorePreference() {
		config->setBool(CONFIG_ASK_SESSION_RESTORE, true);
		config->forceSave();

		LockedMessageBox::showInfo(
			L"Session restore preference has been reset.\n\n"
			L"You will be asked about session restoration on next startup.",
			L"Preference Reset"
		);
	}

	bool getAskSessionRestore() const {
		return config->getBool(CONFIG_ASK_SESSION_RESTORE, true);
	}

	void openSettingsDialog() {
		bool askSessionRestore = getAskSessionRestore();
		bool showSessionSuccess = config->getBool(CONFIG_SHOW_SESSION_SUCCESS, false);

		std::wstring message = L"SETTINGS\n\n";
		message += L"Current Settings:\n";
		message += L"• Smoothing: " + std::wstring(useSmoothing ? L"ON" : L"OFF") + L"\n";
		message += L"• Ask Session Restore: " + std::wstring(askSessionRestore ? L"ON" : L"OFF") + L"\n";
		message += L"• Show Session Success Dialog: " + std::wstring(showSessionSuccess ? L"ON" : L"OFF") + L"\n\n";

		message += L"Quick Actions:\n";
		message += L"• Press Q to toggle smoothing\n";
		message += L"• Press R to select new manga folder\n\n";

		message += L"Session Options:\n";
		message += L"YES - Open advanced session settings\n";
		message += L"NO - Close settings\n";

		int result = LockedMessageBox::showQuestion(message, L"Settings");

		if (result == IDYES)
		{
			showAdvancedSessionSettings();
		}
	}

	void showAdvancedSessionSettings() {
		bool askSessionRestore = getAskSessionRestore();
		bool showSessionSuccess = config->getBool(CONFIG_SHOW_SESSION_SUCCESS, false);

		std::wstring message = L"ADVANCED SESSION SETTINGS\n\n";
		message += L"Current Settings:\n";
		message += L"• Ask Session Restore: " + std::wstring(askSessionRestore ? L"ON" : L"OFF") + L"\n";
		message += L"• Show Success Dialog: " + std::wstring(showSessionSuccess ? L"ON" : L"OFF") + L"\n\n";

		message += L"What would you like to change?\n\n";
		message += L"1. Toggle 'Ask Session Restore'\n";
		message += L"2. Toggle 'Show Success Dialog'\n";
		message += L"3. Reset all session preferences\n";
		message += L"4. Cancel";

		// Use a custom dialog with numbered options
		int choice = showNumberedChoiceDialog(message, L"Advanced Session Settings", 4);

		switch (choice)
		{
		case 1:
			config->setBool(CONFIG_ASK_SESSION_RESTORE, !askSessionRestore);
			config->forceSave();
			LockedMessageBox::showInfo(
				L"Ask Session Restore: " + std::wstring(!askSessionRestore ? L"ENABLED" : L"DISABLED"),
				L"Setting Updated"
			);
			break;

		case 2:
			config->setBool(CONFIG_SHOW_SESSION_SUCCESS, !showSessionSuccess);
			config->forceSave();
			LockedMessageBox::showInfo(
				L"Show Success Dialog: " + std::wstring(!showSessionSuccess ? L"ENABLED" : L"DISABLED"),
				L"Setting Updated"
			);
			break;

		case 3:
			resetAllSessionPreferences();
			break;

		case 4:
		default:
			// Cancel - do nothing
			break;
		}
	}

	void resetAllSessionPreferences() {
		config->setBool(CONFIG_ASK_SESSION_RESTORE, true);
		config->setBool(CONFIG_SHOW_SESSION_SUCCESS, false);
		config->forceSave();

		LockedMessageBox::showInfo(
			L"All session preferences have been reset to defaults:\n\n"
			L"• Ask Session Restore: ENABLED\n"
			L"• Show Success Dialog: DISABLED\n\n"
			L"Changes will take effect on next startup.",
			L"Preferences Reset"
		);
	}

	void setAskSessionRestore(bool ask) {
		config->setBool(CONFIG_ASK_SESSION_RESTORE, ask);
		config->forceSave();
	}

	int showNumberedChoiceDialog(const std::wstring& message, const std::wstring& title, int maxChoice) {
		std::wstring fullMessage = message + L"\n\nEnter choice (1-" + std::to_wstring(maxChoice) + L"):";

		// For simplicity, use a series of Yes/No dialogs
		// In a more advanced implementation, you might create a custom dialog
		std::wstring choiceMsg = message + L"\n\nChoose option 1? (Press NO to see next option)";

		for (int i = 1; i <= maxChoice; i++)
		{
			std::wstring currentMsg = message + L"\n\nChoose option " + std::to_wstring(i) + L"?";
			if (i < maxChoice)
			{
				currentMsg += L"\n(Press NO to see next option, CANCEL to abort)";
			}

			int result = LockedMessageBox::showMessageBox(currentMsg, title, MB_YESNOCANCEL | MB_ICONQUESTION);

			if (result == IDYES)
			{
				return i;
			}
			else if (result == IDCANCEL)
			{
				return maxChoice; // Return last option (usually cancel)
			}
			// If NO, continue to next option
		}

		return maxChoice; // Default to last option
	}

	void updateNavigationButtons() {
		// Enable/disable buttons based on available folders
		bool hasMultipleFolders = (folders.size() > 1);

		buttonManager.enableButton(ButtonID::PREVIOUS_FOLDER, hasMultipleFolders);
		buttonManager.enableButton(ButtonID::NEXT_FOLDER, hasMultipleFolders);
	}

	void updateAllButtonPositions() {
		// Use lambda to calculate positions for each button
		buttonManager.updateAllPositions([this](ButtonID id) -> sf::Vector2f {
			float buttonY = 10.0f;
			float spacing = 35.0f;
			float infoButtonX = static_cast<float>(window.getSize().x) - 50.0f;

			switch (id)
			{
			case ButtonID::SETTINGS_BUTTON:
				return sf::Vector2f(infoButtonX - (spacing * 4), buttonY);
			case ButtonID::HELP_BUTTON:
				return sf::Vector2f(infoButtonX - (spacing * 3), buttonY);
			case ButtonID::INFO_BUTTON:
				return sf::Vector2f(infoButtonX, buttonY);
			case ButtonID::NEXT_FOLDER:
				return sf::Vector2f(infoButtonX - spacing, buttonY);
			case ButtonID::PREVIOUS_FOLDER:
				return sf::Vector2f(infoButtonX - (spacing * 2), buttonY);
			default:
				return sf::Vector2f(0, 0);
			}
			});

		updateNavigationButtons();
	}

	void handleButtonClick(ButtonID clickedButton) {
		switch (clickedButton)
		{
		case ButtonID::INFO_BUTTON:
			buttonManager.toggleButton(ButtonID::INFO_BUTTON);
			saveCurrentSession(); // Save when UI changes
			break;

		case ButtonID::HELP_BUTTON:
			buttonManager.toggleButton(ButtonID::HELP_BUTTON);
			showHelpText = buttonManager.isButtonToggled(ButtonID::HELP_BUTTON);
			saveCurrentSession();
			break;

		case ButtonID::PREVIOUS_FOLDER:
			if (navLock.isNavigationAllowed())
			{
				previousFolder();
				saveCurrentSession(); // Save after navigation
			}
			break;

		case ButtonID::NEXT_FOLDER:
			if (navLock.isNavigationAllowed())
			{
				nextFolder();
				saveCurrentSession(); // Save after navigation
			}
			break;
		case ButtonID::SETTINGS_BUTTON:
			// Handle settings button click
			openSettingsDialog(); // Your settings method
			break;
		case ButtonID::COUNT:
		default:
			// No valid button clicked or unhandled button
			break;
		}
	}

	void updateButtonStatesExample() {
		// Enable/disable based on conditions
		buttonManager.enableButton(ButtonID::NEXT_FOLDER, currentFolderIndex < folders.size() - 1);
		buttonManager.enableButton(ButtonID::PREVIOUS_FOLDER, currentFolderIndex > 0);

		// Check button states
		bool infoToggled = buttonManager.isButtonToggled(ButtonID::INFO_BUTTON);

		// Get button for direct access
		UIButton* infoBtn = buttonManager.getButton(ButtonID::INFO_BUTTON);
		if (infoBtn)
		{
			sf::Vector2f pos = infoBtn->getPosition();
			// Use position for something...
		}
	}

	SessionChoice showSessionRestoreDialog() {
		// Check if we should ask (default: true for new users)
		bool shouldAsk = config->getBool(CONFIG_ASK_SESSION_RESTORE, true);

		if (!shouldAsk)
		{
			// If set to not ask, always restore session if available
			return SessionChoice::RESTORE_SESSION;
		}

		std::wstring message = L"PREVIOUS SESSION DETECTED\n\n";

		// Get session details if available
		std::wstring lastFolder = config->getWString(CONFIG_LAST_FOLDER, L"");
		int lastFolderIndex = config->getInt(CONFIG_LAST_FOLDER_INDEX, 0);
		int lastImageIndex = config->getInt(CONFIG_LAST_IMAGE_INDEX, 0);

		if (!lastFolder.empty())
		{
			std::filesystem::path folderPath(lastFolder);
			std::wstring folderName = folderPath.filename().wstring();
			if (folderName.empty())
			{
				folderName = folderPath.parent_path().filename().wstring();
			}

			message += L"Last Folder: " + folderName + L"\n";
			message += L"Folder Position: " + std::to_wstring(lastFolderIndex + 1) + L"\n";
			message += L"Image Position: " + std::to_wstring(lastImageIndex + 1) + L"\n\n";
		}

		message += L"Would you like to:\n\n";
		message += L"YES - Continue from where you left off\n";
		message += L"NO - Start with folder selection dialog\n";
		message += L"CANCEL - Exit application\n\n";
		message += L"(You can change this preference later in settings)";

		int result = LockedMessageBox::showMessageBox(message, L"Restore Previous Session?",
			MB_YESNOCANCEL | MB_ICONQUESTION | MB_DEFBUTTON1);

		switch (result)
		{
		case IDYES:
			// Ask about future behavior only if they chose to restore
			askAboutFutureSessionBehavior();
			return SessionChoice::RESTORE_SESSION;

		case IDNO:
			// Ask about future behavior since they chose new session
			askAboutFutureSessionBehavior();
			return SessionChoice::NEW_SESSION;

		case IDCANCEL:
		default:
			return SessionChoice::CANCELLED;
		}
	}

	void askAboutFutureSessionBehavior() {
		std::wstring message = L"SESSION PREFERENCE\n\n";
		message += L"Would you like to be asked about session restoration in the future?\n\n";
		message += L"YES - Always ask (current behavior)\n";
		message += L"NO - Always restore previous session automatically\n\n";
		message += L"Note: You can change this setting by editing the configuration file or through the settings menu.";

		int result = LockedMessageBox::showQuestion(message, L"Future Session Behavior");

		if (result == IDNO)
		{
			// User chose to not be asked again - set config to auto-restore
			config->setBool(CONFIG_ASK_SESSION_RESTORE, false);
			config->forceSave();

			LockedMessageBox::showInfo(
				L"Session preference updated.\n\n"
				L"The application will now automatically restore your previous session on startup.\n\n"
				L"You can change this by editing the configuration file:\n" +
				config->getConfigFilePath(),
				L"Preference Saved"
			);
		}
		// If YES (or other), keep the default behavior of asking
	}

	bool hasValidPreviousSession() {
		// Check if we have the minimum required session data
		if (!config->hasKey(CONFIG_LAST_FOLDER))
		{
			return false;
		}

		std::wstring lastFolder = config->getWString(CONFIG_LAST_FOLDER);
		if (lastFolder.empty())
		{
			return false;
		}

		// Verify the folder still exists
		try
		{
			return std::filesystem::exists(lastFolder);
		} catch (...)
		{
			return false;
		}
	}

	void markSessionAsActive() {
		// Mark that we have an active session for future checks
		config->setBool(CONFIG_LAST_SESSION_EXISTS, true);
		config->forceSave();
	}

	void initializeConfig() {
		try
		{
			config = std::make_unique<ConfigManager>();

			// Check if we have a valid previous session
			if (hasValidPreviousSession())
			{
				SessionChoice choice = showSessionRestoreDialog();

				switch (choice)
				{
				case SessionChoice::RESTORE_SESSION:
					if (attemptSessionRestore())
					{
						markSessionAsActive();
						return; // Successfully restored
					}
					else
					{
						// Restore failed, fall back to folder selection
						LockedMessageBox::showWarning(
							L"Failed to restore previous session.\n"
							L"Starting with folder selection dialog.",
							L"Session Restore Failed"
						);
						browseFolderOnStartup();
					}
					break;

				case SessionChoice::NEW_SESSION:
					// User explicitly chose new session
					browseFolderOnStartup();
					break;

				case SessionChoice::CANCELLED:
					// User cancelled - close application
					window.close();
					return;
				}
			}
			else
			{
				// No valid previous session, start fresh
				browseFolderOnStartup();
			}

		} catch (const std::exception& e)
		{
			// If anything fails, fall back to normal startup
			LockedMessageBox::showWarning(
				L"Error during initialization: " + UnicodeUtils::stringToWstring(e.what()) +
				L"\n\nStarting with folder selection.",
				L"Initialization Error"
			);
			browseFolderOnStartup();
		}
	}

	bool attemptSessionRestore() {
		try
		{
			std::wstring lastFolder = config->getWString(CONFIG_LAST_FOLDER);
			int lastFolderIndex = config->getInt(CONFIG_LAST_FOLDER_INDEX, 0);
			int lastImageIndex = config->getInt(CONFIG_LAST_IMAGE_INDEX, 0);

			if (lastFolder.empty() || !std::filesystem::exists(lastFolder))
			{
				return false;
			}

			rootMangaPath = lastFolder;
			loadFolders(rootMangaPath);

			updateNavigationButtons();

			if (!folders.empty())
			{
				// Clamp indices to valid ranges
				currentFolderIndex = std::max(0, std::min(lastFolderIndex, (int)folders.size() - 1));

				loadImagesFromFolder(folders[currentFolderIndex]);

				if (!currentImages.empty())
				{
					currentImageIndex = std::max(0, std::min(lastImageIndex, (int)currentImages.size() - 1));
					if (loadCurrentImage())
					{
						// Restore UI states
						restoreUIStates();
						updateWindowTitle();

						// Only show success message if enabled (default: false)
						bool showSuccess = config->getBool(CONFIG_SHOW_SESSION_SUCCESS, false);
						if (showSuccess)
						{
							std::wstring successMsg = L"Session restored successfully!\n\n";
							std::filesystem::path folderPath(lastFolder);
							std::wstring folderName = folderPath.filename().wstring();
							if (folderName.empty())
							{
								folderName = folderPath.parent_path().filename().wstring();
							}

							successMsg += L"Folder: " + folderName + L"\n";
							successMsg += L"Position: " + std::to_wstring(currentFolderIndex + 1) +
								L"/" + std::to_wstring(folders.size()) + L" folders, " +
								std::to_wstring(currentImageIndex + 1) + L"/" +
								std::to_wstring(currentImages.size()) + L" images";

							LockedMessageBox::showInfo(successMsg, L"Session Restored");
						}

						return true;
					}
				}
			}

			return false;

		} catch (const std::exception& e)
		{
			return false;
		}
	}

	void restoreUIStates() {
		// Restore UI button states
		if (config->hasKey("UI.infoButtonVisible"))
		{
			bool infoVisible = config->getBool("UI.infoButtonVisible", false);
			buttonManager.getButton(ButtonID::INFO_BUTTON)->setToggleState(true, infoVisible);
		}

		if (config->hasKey("UI.helpButtonVisible"))
		{
			bool helpVisible = config->getBool("UI.helpButtonVisible", true);
			buttonManager.getButton(ButtonID::HELP_BUTTON)->setToggleState(true, helpVisible);
			showHelpText = helpVisible;
		}
	}
public: //helpers

	sf::Image scaleImage(const sf::Image& originalImage, sf::Vector2u newSize) {
		sf::Vector2u originalSize = originalImage.getSize();

		if (originalSize == newSize)
		{
			return originalImage;
		}

		sf::Image scaledImage(newSize);

		float scaleX = static_cast<float>(originalSize.x) / static_cast<float>(newSize.x);
		float scaleY = static_cast<float>(originalSize.y) / static_cast<float>(newSize.y);

		if (useSmoothing)
		{
			// Bilinear interpolation for better quality
			for (unsigned int y = 0; y < newSize.y; ++y)
			{
				for (unsigned int x = 0; x < newSize.x; ++x)
				{
					float srcX = x * scaleX;
					float srcY = y * scaleY;

					// Get surrounding pixels
					int x1 = static_cast<int>(srcX);
					int y1 = static_cast<int>(srcY);
					int x2 = std::min(x1 + 1, static_cast<int>(originalSize.x - 1));
					int y2 = std::min(y1 + 1, static_cast<int>(originalSize.y - 1));

					// Calculate interpolation weights
					float fx = srcX - x1;
					float fy = srcY - y1;

					// Get pixel colors
					sf::Color c11 = originalImage.getPixel(sf::Vector2u(x1, y1));
					sf::Color c21 = originalImage.getPixel(sf::Vector2u(x2, y1));
					sf::Color c12 = originalImage.getPixel(sf::Vector2u(x1, y2));
					sf::Color c22 = originalImage.getPixel(sf::Vector2u(x2, y2));

					// Bilinear interpolation
					std::uint8_t r = static_cast<std::uint8_t>(
						c11.r * (1 - fx) * (1 - fy) + c21.r * fx * (1 - fy) +
						c12.r * (1 - fx) * fy + c22.r * fx * fy);
					std::uint8_t g = static_cast<std::uint8_t>(
						c11.g * (1 - fx) * (1 - fy) + c21.g * fx * (1 - fy) +
						c12.g * (1 - fx) * fy + c22.g * fx * fy);
					std::uint8_t b = static_cast<std::uint8_t>(
						c11.b * (1 - fx) * (1 - fy) + c21.b * fx * (1 - fy) +
						c12.b * (1 - fx) * fy + c22.b * fx * fy);
					std::uint8_t a = static_cast<std::uint8_t>(
						c11.a * (1 - fx) * (1 - fy) + c21.a * fx * (1 - fy) +
						c12.a * (1 - fx) * fy + c22.a * fx * fy);

					scaledImage.setPixel(sf::Vector2u(x, y), sf::Color(r, g, b, a));
				}
			}
		}
		else
		{
			// Nearest neighbor (faster but lower quality)
			for (unsigned int y = 0; y < newSize.y; ++y)
			{
				for (unsigned int x = 0; x < newSize.x; ++x)
				{
					int srcX = static_cast<int>(x * scaleX);
					int srcY = static_cast<int>(y * scaleY);

					srcX = std::min(srcX, static_cast<int>(originalSize.x - 1));
					srcY = std::min(srcY, static_cast<int>(originalSize.y - 1));

					scaledImage.setPixel(sf::Vector2u(x, y), originalImage.getPixel(sf::Vector2u(srcX, srcY)));
				}
			}
		}

		return scaledImage;
	}

	// Get image dimensions as string
	std::string getImageDimensionsString() {
		if (originalTexture.getSize().x == 0 || originalTexture.getSize().y == 0)
		{
			return "Unknown";
		}

		sf::Vector2u size = originalTexture.getSize();
		return std::to_string(size.x) + " x " + std::to_string(size.y) + " pixels";
	}

	sf::Vector2u getImageDimensions(int imageIndex) {
		if (imageIndex < 0 || imageIndex >= currentImages.size())
		{
			return sf::Vector2u(0, 0);
		}

		ImageLoadingDispatcher::LoadContext context(isCurrentlyInArchive, &archiveHandler, &currentImages, imageIndex);
		return ImageLoadingDispatcher::getImageDimensionsAtIndex(context);
	}

	bool isArchiveFile(const std::string& ext) {
		std::string cpy = ext;
		std::transform(cpy.begin(), cpy.end(), cpy.begin(), ::tolower);
		for (auto& supp : supportedArchives)
		{
			if (cpy == supp)
			{
				return true;
			}
		}
		return false;
	}

public: //loads

	bool tryNextValidFolder() {
		if (folders.empty()) return false;

		int originalFolderIndex = currentFolderIndex;
		int attempts = 0;

		// Try up to all folders to find a working one
		while (attempts < folders.size())
		{
			currentFolderIndex = (currentFolderIndex + 1) % folders.size();
			attempts++;

			// Avoid infinite loop back to original failed folder
			if (currentFolderIndex == originalFolderIndex && attempts > 1)
			{
				break;
			}

			loadImagesFromFolder(folders[currentFolderIndex]);

			if (!currentImages.empty())
			{
				currentImageIndex = 0;
				if (loadCurrentImage())
				{
					return true; // Successfully loaded a working folder
				}
			}
		}

		updateNavigationButtons(); // Update button states when folder changes
		return false; // No working folders found
	}

	void loadFolders(const std::wstring& path) {
		folders.clear();

		try
		{
			for (const auto& entry : std::filesystem::directory_iterator(path))
			{
				if (entry.is_directory())
				{
					// Check if directory contains images
					bool hasImages = false;
					for (const auto& subEntry : std::filesystem::directory_iterator(entry.path()))
					{
						if (subEntry.is_regular_file() && IsImgExtValid(subEntry.path().extension().string()))
						{
							hasImages = true;
							break;
						}
					}

					if (hasImages)
					{
						folders.emplace_back(entry.path().wstring(), false);
					}
				}
				else if (entry.is_regular_file())
				{
					// Check if it's an archive file
					if (isArchiveFile(entry.path().extension().string()))
					{
						folders.emplace_back(entry.path().wstring(), true);
					}
				}
			}

			// Also add current directory if it has images
			bool hasImages = false;
			for (const auto& entry : std::filesystem::directory_iterator(path))
			{
				if (entry.is_regular_file() && IsImgExtValid(entry.path().extension().string()))
				{
					hasImages = true;
					break;
				}
			}

			if (hasImages)
			{
				folders.insert(folders.begin(), std::move(FoldersIdent(path, false)));
			}

			std::sort(folders.begin(), folders.end());

		} catch (const std::exception& e)
		{
			std::wstring errorMsg = L"Error loading folders: " + UnicodeUtils::stringToWstring(e.what());
			LockedMessageBox::showWarning(errorMsg, L"Folder Loading Error");
		}
	}

	void loadImagesFromFolder(const FoldersIdent& folderIdent) {
		// Wait for any ongoing loading to complete
		if (folderLoadingFuture.valid())
		{
			folderLoadingFuture.wait();
		}

		currentImages.clear();
		currentImageIndex = 0;

		// Close any existing archive before opening new one
		if (isCurrentlyInArchive && archiveHandler.getIsArchiveOpen())
		{
			archiveHandler.closeArchive();
		}

		// Reset archive state
		isCurrentlyInArchive = false;
		currentArchivePath = L"";

		// Reset zoom and position only when changing folders
		resetZoomAndPosition();
		sizeMismatchHandler.reset();

		try
		{
			if (folderIdent.isArchieve)
			{
				// Load from archive
				if (archiveHandler.openArchive(folderIdent.dir))
				{
					const auto& entries = archiveHandler.getImageEntries();

					if (entries.empty())
					{
						// Archive opened but no images found
						std::wstring errorMsg = L"No images found in archive: " + folderIdent.dir;
						LockedMessageBox::showError(errorMsg, L"No Images in Archive");
						return;
					}

					for (const auto& entry : entries)
					{
						// Store archive entries as pseudo-paths for consistency
						std::wstring entryPath = folderIdent.dir + L"#" + UnicodeUtils::stringToWstring(entry.name);
						currentImages.push_back(entryPath);
					}
					isCurrentlyInArchive = true;
					currentArchivePath = folderIdent.dir;
				}
				else
				{
					// Failed to open archive
					std::wstring errorMsg = L"Failed to open archive: " + folderIdent.dir;
					LockedMessageBox::showError(errorMsg, L"Archive Error");
					return;
				}
			}
			else
			{
				// Load from regular folder
				try
				{
					for (const auto& entry : std::filesystem::directory_iterator(folderIdent.dir))
					{
						if (entry.is_regular_file() && IsImgExtValid(entry.path().extension().string()))
						{
							currentImages.push_back(entry.path().wstring());
						}
					}

					// Sort images naturally
					std::sort(currentImages.begin(), currentImages.end());

				} catch (const std::filesystem::filesystem_error& e)
				{
					std::wstring errorMsg = L"Error accessing folder: " + folderIdent.dir + L"\n" + UnicodeUtils::stringToWstring(e.what());
					LockedMessageBox::showError(errorMsg, L"Folder Access Error");
					return;
				}
			}

			// Start loading all images in background
			if (!currentImages.empty())
			{
				loadAllImagesInFolder();
				updateWindowTitle();
			}
			else
			{
				std::wstring source = folderIdent.isArchieve ? L"archive" : L"folder";
				std::wstring errorMsg = L"No images found in " + source + L": " + folderIdent.dir;
				LockedMessageBox::showError(errorMsg, L"No Images Found");
			}

		} catch (const std::exception& e)
		{
			std::wstring errorMsg = L"Error loading images from folder '" + folderIdent.dir + L"': " + UnicodeUtils::stringToWstring(e.what());
			LockedMessageBox::showError(errorMsg, L"Image Loading Error");
			std::exit(0u);
		}
	}

	void loadSingleImageAsync(int index) {
		if (index < 0 || index >= currentImages.size()) return;

		ImageLoadingDispatcher::LoadContext context(isCurrentlyInArchive, &archiveHandler, &currentImages, index);
		ImageLoader::LoadResult result = ImageLoadingDispatcher::loadImageAtIndex(context);

		if (result.success)
		{
			std::lock_guard<std::mutex> lock(loadingMutex);
			loadedImages[index].image = std::move(result.image);
			loadedImages[index].filename = FileSystemHelper::extractFilenameFromPath(currentImages[index], isCurrentlyInArchive);
			loadedImages[index].isLoaded = true;

			// Get file size
			if (!isCurrentlyInArchive)
			{
				try
				{
					loadedImages[index].fileSize = std::filesystem::file_size(currentImages[index]);
				} catch (...)
				{
					loadedImages[index].fileSize = 0;
				}
			}
		}
	}

	void loadImagesAsync() {
		const int totalImages = currentImages.size();
		std::vector<std::future<void>> workers;
		const int numThreads = std::min(4, (int)std::thread::hardware_concurrency());
		const int imagesPerThread = totalImages / numThreads;

		for (int t = 0; t < numThreads; ++t)
		{
			int startIdx = t * imagesPerThread;
			int endIdx = (t == numThreads - 1) ? totalImages : (t + 1) * imagesPerThread;

			workers.emplace_back(std::async(std::launch::async, [this, startIdx, endIdx]() {
				for (int i = startIdx; i < endIdx; ++i)
				{
					loadSingleImageAsync(i);
					loadingProgress = loadingProgress + 1;
				}
				}));
		}

		// Wait for all workers to complete
		for (auto& worker : workers)
		{
			worker.wait();
		}

		isLoadingFolder = false;
	}

	void loadAllImagesInFolder() {
		if (currentImages.empty()) return;

		isLoadingFolder = true;
		loadingProgress = 0;

		// LOCK navigation during async loading
		navLock.lock("Loading Images");

		// Clear previous data
		{
			std::lock_guard<std::mutex> lock(loadingMutex);
			loadedImages.clear();
			loadedImages.resize(currentImages.size());
		}

		// Start async loading
		folderLoadingFuture = std::async(std::launch::async, [this]() {
			loadImagesAsync();
			// UNLOCK navigation when async loading is complete
			navLock.unlock();
			});
	}

	bool loadCurrentImage() {
		if (currentImages.empty()) return false;

		// Check if we're still loading
		if (isLoadingFolder)
		{
			// Show loading progress and try to load synchronously
			updateLoadingProgress();

			// Try to load the current image synchronously while others load in background
			sf::Image imageData;
			bool loaded = false;

			ImageLoadingDispatcher::LoadContext context(isCurrentlyInArchive, &archiveHandler, &currentImages, currentImageIndex);
			ImageLoader::LoadResult result = ImageLoadingDispatcher::loadImageAtIndex(context);
			if (result.success)
			{
				imageData = std::move(result.image);
				loaded = true;
			}
			else
			{
				loaded = false;
				// Show error if needed: LockedMessageBox::showError(UnicodeUtils::stringToWstring(result.errorMessage), L"Image Loading Error");
			}

			if (loaded)
			{
				setupTextureFromImage(imageData);
				updateWindowTitle();
				return true;
			}

			return false;
		}

		// Use preloaded data if available
		{
			std::lock_guard<std::mutex> lock(loadingMutex);
			if (currentImageIndex < loadedImages.size() &&
				loadedImages[currentImageIndex].isLoaded)
			{

				setupTextureFromImage(loadedImages[currentImageIndex].image);
				updateWindowTitle();
				return true;
			}
		}

		sf::Image imageData;
		bool loaded = false;

		ImageLoadingDispatcher::LoadContext context(isCurrentlyInArchive, &archiveHandler, &currentImages, currentImageIndex);
		ImageLoader::LoadResult result = ImageLoadingDispatcher::loadImageAtIndex(context);
		if (result.success)
		{
			imageData = std::move(result.image);
			loaded = true;
		}
		else
		{
			loaded = false;
			// Show error if needed: LockedMessageBox::showError(UnicodeUtils::stringToWstring(result.errorMessage), L"Image Loading Error");
		}

		if (loaded)
		{
			setupTextureFromImage(imageData);
			updateWindowTitle();
			return true;
		}

		// Show error if image fails to load
		std::wstring imagePath = currentImages[currentImageIndex];
		std::wstring errorMsg = L"Failed to load image: " + imagePath;
		LockedMessageBox::showError(errorMsg, L"Image Loading Error");
		return false;
	}

public://setup

	void updateHelpTextPosition() {
		if (!helpText.get()) return;

		sf::Vector2u windowSize = window.getSize();
		sf::FloatRect textBounds = helpText.get()->getLocalBounds();

		// Position help text with 20px margin from bottom
		float yPosition = static_cast<float>(windowSize.y) - textBounds.size.y - 20.0f;

		// Ensure it doesn't go above the window (keep at least 200px from top)
		yPosition = std::max(yPosition, 200.0f);

		// In fullscreen mode, you might want different positioning
		if (isCurrentlyFullscreen)
		{
			// Optional: Different positioning logic for fullscreen
			// For example, more margin from edges in fullscreen
			yPosition = static_cast<float>(windowSize.y) - textBounds.size.y - 40.0f;
			yPosition = std::max(yPosition, 250.0f);
		}

		helpText.get()->setPosition(sf::Vector2f(10.0f, yPosition));
	}

	void setupUI() {
		statusText.initialize(*font.get(), 20u);
		statusText.get()->setFillColor(sf::Color::White);
		statusText.get()->setPosition(sf::Vector2f(10.0f, 10.0f));

		helpText.initialize(*font.get(), 16u);
		helpText.get()->setFillColor(sf::Color::Yellow);
		helpText.get()->setPosition(sf::Vector2f(10.0f, static_cast<float>(window.getSize().y) - 180.0f));
		helpText.get()->setString(
			"Controls:\n"
			"Mouse Wheel: Navigate images\n"
			"Ctrl + Mouse Wheel: Zoom\n"
			"Middle Click: Reset zoom\n"
			"Arrow Keys/WASD: Pan image\n"
			"Tab: Toggle folder\n"
			"F: Fit to window\n"
			"C: Center image (keep zoom)\n"
			"H: Toggle help\n"
			"I: Toggle detailed info\n"
			"R: Select new manga folder\n"
			"F10: Toggle maximize (windowed mode)\n"    // NEW
			"F11: Toggle fullscreen (exclusive mode)\n" // UPDATED
			"Left Click Info Button: Toggle info\n"
			"Navigation Buttons: < (prev folder) > (next folder) Info"
		);

		updateHelpTextPosition();

		// Setup detailed info text
		detailedInfoText.initialize(*font.get(), 14u);
		detailedInfoText.get()->setFillColor(sf::Color::Cyan);
		detailedInfoText.get()->setPosition(sf::Vector2f(10.0f, 120.0f));

		// Initialize UI button positions
		initializeButtons();
	}

	void handleZoom(float zoomDelta) {
		sf::Vector2f mousePos = sf::Vector2f(sf::Mouse::getPosition(window));
		sf::Vector2f worldMousePos = window.mapPixelToCoords(sf::Vector2i(static_cast<int>(mousePos.x),
			static_cast<int>(mousePos.y)));

		// Get current image position
		sf::Vector2f oldImagePos = currentSprite.get()->getPosition();

		// Calculate zoom
		float oldZoom = zoomLevel;
		if (zoomDelta > 0)
		{
			zoomLevel *= 1.1f;
		}
		else
		{
			zoomLevel *= 0.9f;
		}
		zoomLevel = std::max(0.1f, std::min(5.0f, zoomLevel));

		// Mark as custom zoom
		savedZoomLevel = zoomLevel;
		hasCustomZoom = true;

		// Update scaled texture for new zoom level
		updateScaledTexture();

		// Set sprite scale
		float spriteScale = (zoomLevel <= 1.0f) ? 1.0f : zoomLevel;
		currentSprite.get()->setScale(sf::Vector2f(spriteScale, spriteScale));

		// Zoom towards mouse position
		sf::Vector2f newImagePos = oldImagePos;
		if (oldZoom != zoomLevel)
		{
			float zoomFactor = zoomLevel / oldZoom;
			sf::Vector2f mouseToImage = oldImagePos - worldMousePos;
			newImagePos = worldMousePos + mouseToImage * zoomFactor;
		}

		currentSprite.get()->setPosition(newImagePos);
		imagePosition = newImagePos;

		// Update saved offset from center
		updateSavedOffset();

		updateStatusText();
		updateDetailedInfo();
	}

	void setupTextureFromImage(const sf::Image& imageData) {
		scaledTexture = sf::Texture();

		if (originalTexture.loadFromImage(imageData))
		{
			originalTexture.setSmooth(useSmoothing);
			// Update the size tracker (no need for reset check here anymore)
			sf::Vector2u currentImageSize = originalTexture.getSize();
			bool needsReset = sizeMismatchHandler.shouldResetZoom(currentImageSize);

			if (needsReset) {
				// Reset zoom and position for size mismatch
				resetZoomAndPosition();
				if (currentSprite.get()) {
					currentSprite.get()->setScale(sf::Vector2f(1.0f, 1.0f));
				}
			}

			lastZoomLevel = -1.0f;
			updateScaledTexture();
			// Apply current zoom and position (don't reset)
			fitToWindow(needsReset); // false = don't force reset
			// debugCurrentScaling();
			updateStatusText();
			updateDetailedInfo();

			// Preload next images if using preloading system
			if (isCurrentlyInArchive) {
				archiveHandler.preloadImages(currentImageIndex);
			}
		}
	}

	void handleWindowResize(sf::Vector2u newSize) {
		// Update view
		currentView.setSize({ static_cast<float>(newSize.x), static_cast<float>(newSize.y) });
		currentView.setCenter({ static_cast<float>(newSize.x) / 2.0f, static_cast<float>(newSize.y) / 2.0f });
		window.setView(currentView);

		HWND hwnd = window.getNativeHandle();
		if (hwnd)
		{
			bool nowMaximized = IsZoomed(hwnd) == TRUE;
			if (nowMaximized != isCurrentlyMaximized)
			{
				isCurrentlyMaximized = nowMaximized;
				// Optionally save immediately on state change
				saveCurrentSession();
			}
		}

		// Update all button positions in batch
		updateAllButtonPositions();
		updateHelpTextPosition();

		// Refit image with current zoom preferences
		if (originalTexture.getSize().x > 0)
		{
			fitToWindow(false); // Don't force reset, maintain user preferences
		}

		lastWindowSize = newSize;
	}

public: //update funcs

	void updateSavedOffset() {
		sf::Vector2u windowSize = window.getSize();
		sf::Vector2f windowCenter(static_cast<float>(windowSize.x) / 2.0f,
			static_cast<float>(windowSize.y) / 2.0f);

		sf::FloatRect spriteBounds = currentSprite.get()->getGlobalBounds();
		sf::Vector2f imageCenter(spriteBounds.position.x + spriteBounds.size.x / 2.0f,
			spriteBounds.position.y + spriteBounds.size.y / 2.0f);

		savedImageOffset = imageCenter - windowCenter;
		hasCustomPosition = (savedImageOffset.x != 0 || savedImageOffset.y != 0);
	}

	void updateScaledTexture() {
		if (originalTexture.getSize().x == 0 || originalTexture.getSize().y == 0)
		{
			return;
		}

		sf::Vector2u originalSize = originalTexture.getSize();
		sf::Vector2u windowSize = window.getSize();

		// Calculate target size based on zoom and window size
		sf::Vector2u targetSize;

		if (zoomLevel <= 1.0f)
		{
			// Downscaling: calculate optimal size to maintain quality
			targetSize.x = static_cast<unsigned int>(originalSize.x * zoomLevel);
			targetSize.y = static_cast<unsigned int>(originalSize.y * zoomLevel);

			// Ensure minimum size for readability
			targetSize.x = std::max(targetSize.x, 100u);
			targetSize.y = std::max(targetSize.y, 100u);
		}
		else
		{
			// For upscaling, use original texture and let GPU handle it
			scaledTexture = originalTexture;
			currentSprite.initialize(scaledTexture);
			return;
		}

		// Only rescale if significant change in zoom or window size
		bool needsRescale = (std::abs(zoomLevel - lastZoomLevel) > 0.1f) ||
			(windowSize != lastWindowSize) ||
			(scaledTexture.getSize().x == 0);

		if (needsRescale)
		{
			sf::Image originalImage = originalTexture.copyToImage();
			sf::Image scaledImage = scaleImage(originalImage, targetSize);

			if (scaledTexture.loadFromImage(scaledImage))
			{
				currentSprite.initialize(scaledTexture);
				lastZoomLevel = zoomLevel;
				lastWindowSize = windowSize;
			}
		}
	}

	void updateLoadingProgress() {
		if (!isLoadingFolder) return;

		int progress = loadingProgress;
		int total = currentImages.size();
		float percentage = (float)progress / (float)total * 100.0f;

		std::string loadingString = "Loading images: " + std::to_string(progress) +
			"/" + std::to_string(total) +
			" (" + std::to_string((int)percentage) + "%)";

		loadingText.get()->setString(UnicodeUtils::stringToSFString(loadingString));
	}

	void updateStatusText() {
		if (!folders.empty() && !currentImages.empty())
		{
			std::string statusString = (
				"Image: " + std::to_string(currentImageIndex + 1) + "/" +
				std::to_string(currentImages.size()) + "\n" +
				"Zoom: " + std::to_string(static_cast<int>(zoomLevel * 100)) + "%" +
				" | Smooth: " + (useSmoothing ? "ON" : "OFF")
				);

			if (isCurrentlyFullscreen)
			{
				statusString += " | Fullscreen";
			}
			else if (isCurrentlyMaximized)
			{
				statusString += " | Maximized";
			}

			if (isCurrentlyInArchive)
			{
				statusString = "[ARCHIVE] " + statusString;
			}

			statusText.get()->setString(UnicodeUtils::stringToSFString(statusString));
		}
	}

	void updateDetailedInfo() {
		if (!folders.empty() && !currentImages.empty())
		{
			const std::string folderPath = UnicodeUtils::wstringToString(folders[currentFolderIndex].dir);
			const std::string imagePath = UnicodeUtils::wstringToString(currentImages[currentImageIndex]);
			std::string fileName;
			std::string extension;
			std::string fileSize;

			if (isCurrentlyInArchive)
			{
				// Extract filename from archive entry path
				size_t hashPos = imagePath.find('#');
				if (hashPos != std::string::npos)
				{
					fileName = imagePath.substr(hashPos + 1);
					extension = std::filesystem::path(fileName).extension().string();
				}
				const auto& entries = archiveHandler.getImageEntries();
				if (currentImageIndex < entries.size()) {
					fileSize = FileSystemHelper::getFileSizeString(entries[currentImageIndex].size);
				}
			}
			else
			{
				fileName = UnicodeUtils::getFilenameOnly(imagePath);
				std::filesystem::path path(currentImages[currentImageIndex]);
				extension = UnicodeUtils::wstringToString(path.extension().wstring());
				fileSize = FileSystemHelper::getFileSizeString(currentImages[currentImageIndex]);
			}

			const std::string dimensions = getImageDimensionsString();
			float folderProgress = ((float)(currentFolderIndex + 1) / (float)folders.size()) * 100.0f;
			float imageProgress = ((float)(currentImageIndex + 1) / (float)currentImages.size()) * 100.0f;
			const std::string current = wrapText(std::string("Current ") + (isCurrentlyInArchive ? "Archive: " : "Folder: ") + UnicodeUtils::getFilenameOnly(folderPath),
				detailedInfoText.get()->getFont(), detailedInfoText.get()->getCharacterSize(), 580.f);
			const std::string show_which = isCurrentlyInArchive ?
				"Archive Entry:\n" + wrapText(fileName, detailedInfoText.get()->getFont(), detailedInfoText.get()->getCharacterSize(), 580.f) :
				"Full Image Path:\n" + wrapText(imagePath, detailedInfoText.get()->getFont(), detailedInfoText.get()->getCharacterSize(), 580.f);

			std::string detailedString = (
				"=== DETAILED INFORMATION ===\n" +
				current + "\n" +
				"Source Progress: " + std::to_string((int)folderProgress) + "% (" +
				std::to_string(currentFolderIndex + 1) + "/" + std::to_string(folders.size()) + ")\n" +
				"Image Progress: " + std::to_string((int)imageProgress) + "% (" +
				std::to_string(currentImageIndex + 1) + "/" + std::to_string(currentImages.size()) + ")\n\n" +

				"=== CURRENT IMAGE ===\n" +
				"File Name: " + wrapText(fileName, detailedInfoText.get()->getFont(), detailedInfoText.get()->getCharacterSize(), 580.f) + "\n" +
				"File Format: " + extension + "\n" +
				"File Size: " + fileSize + "\n" +
				"Dimensions: " + dimensions + "\n" +
				"Zoom Level: " + std::to_string((int)(zoomLevel * 100)) + "%\n" +
				"Source Type: " + (isCurrentlyInArchive ? "Archive" : "Folder") + "\n\n" +

				"=== SOURCE STATISTICS ===\n" +
				"Total Images in Source: " + std::to_string(currentImages.size()) + "\n" +
				"Images Remaining: " + std::to_string(currentImages.size() - currentImageIndex - 1) + "\n" +
				"Total Sources: " + std::to_string(folders.size()) + "\n" +
				"Sources Remaining: " + std::to_string(folders.size() - currentFolderIndex - 1) + "\n\n" +

				"=== PATH INFORMATION ===\n" +
				"Full Source Path:\n" + wrapText(folderPath, detailedInfoText.get()->getFont(), detailedInfoText.get()->getCharacterSize(), 580.f) + "\n\n" +
				show_which
				);

			detailedInfoText.get()->setString(UnicodeUtils::stringToSFString(detailedString));
		}
	}

	bool isWindowMaximized() const {
		HWND hwnd = window.getNativeHandle();
		return hwnd ? (IsZoomed(hwnd) == TRUE) : false;
	}

	void toggleMaximize() {
		HWND hwnd = window.getNativeHandle();
		if (!hwnd) return;

		if (isWindowMaximized())
		{
			ShowWindow(hwnd, SW_RESTORE);
			isCurrentlyMaximized = false;
		}
		else
		{
			ShowWindow(hwnd, SW_MAXIMIZE);
			isCurrentlyMaximized = true;
		}
	}

	bool getIsMaximized() const {
		return isCurrentlyMaximized;
	}

public: //navigations and inputs

	void selectNewMangaFolder() {
		std::wstring newPath = browseForFolder();

		if (!newPath.empty())
		{
			rootMangaPath = newPath;
			loadFolders(rootMangaPath);

			if (folders.empty())
			{
				LockedMessageBox::showError(L"No manga folders or archives found in the selected directory.",
					L"No Manga Found");
				window.close();
			}

			currentFolderIndex = 0;
			loadImagesFromFolder(folders[currentFolderIndex]);

			if (!currentImages.empty())
			{
				currentImageIndex = 0;
				loadCurrentImage();
				updateWindowTitle();
			}
			else
			{
				LockedMessageBox::showError(L"No images found in the manga folders or archives.",
					L"No Images Found");
				window.close();
			}
		}
	}

	void toggleSmoothing() {
		useSmoothing = !useSmoothing;
		if (originalTexture.getSize().x > 0)
		{
			originalTexture.setSmooth(useSmoothing);
			// Force rescale with new smoothing setting
			lastZoomLevel = -1.0f; // Force update
			updateScaledTexture();
			updateStatusText();
		}
	}

	void handleScroll(sf::Vector2f delta) {
		imagePosition += delta;
		currentSprite.get()->setPosition(imagePosition);
		updateSavedOffset();
		hasCustomPosition = true;
	}

	void forceCompleteReset() {
		// Nuclear option - reset everything
		zoomLevel = 1.0f;
		savedZoomLevel = 1.0f;
		lastZoomLevel = -1.0f;
		savedImageOffset = sf::Vector2f(0, 0);
		hasCustomZoom = false;
		hasCustomPosition = false;
		scrollOffset = 0.0f;
		imagePosition = sf::Vector2f(0, 0);

		// Clear sprite state
		if (currentSprite.get())
		{
			currentSprite.get()->setScale(sf::Vector2f(1.0f, 1.0f));
			currentSprite.get()->setPosition(sf::Vector2f(0, 0));
		}

		// Force texture regeneration
		scaledTexture = sf::Texture();
	}

	void resetZoomAndPosition() {
		savedZoomLevel = 1.0f;
		savedImageOffset = sf::Vector2f(0, 0);
		hasCustomZoom = false;
		hasCustomPosition = false;
		scrollOffset = 0;
		zoomLevel = 1.0f;
	}

	void centerImage() {
		sf::Vector2u windowSize = window.getSize();
		sf::FloatRect spriteBounds = currentSprite.get()->getGlobalBounds();

		imagePosition.x = (static_cast<float>(windowSize.x) - spriteBounds.size.x) / 2.0f;
		imagePosition.y = (static_cast<float>(windowSize.y) - spriteBounds.size.y) / 2.0f;

		currentSprite.get()->setPosition(imagePosition);
	}

	void fitToWindow(bool forceReset = false) {
		if (originalTexture.getSize().x == 0 || originalTexture.getSize().y == 0) return;

		sf::Vector2u textureSize = originalTexture.getSize();
		sf::Vector2u windowSize = window.getSize();

		// Calculate the fit-to-window zoom for current image
		float scaleX = static_cast<float>(windowSize.x) / static_cast<float>(textureSize.x);
		float scaleY = static_cast<float>(windowSize.y) / static_cast<float>(textureSize.y);
		float fitToWindowZoom = std::min(scaleX, scaleY);

		if (forceReset || !hasCustomZoom)
		{
			// EXPLICIT RESET: Use fit-to-window zoom and clear all custom settings
			zoomLevel = fitToWindowZoom;
			savedZoomLevel = fitToWindowZoom;
			hasCustomZoom = false;

			// Clear any existing sprite scaling
			if (currentSprite.get())
			{
				currentSprite.get()->setScale(sf::Vector2f(1.0f, 1.0f));
			}
		}
		else
		{
			// Use saved custom zoom level
			zoomLevel = savedZoomLevel;
		}

		// FORCE regenerate scaled texture
		lastZoomLevel = -1.0f; // Force update
		updateScaledTexture();

		// Set sprite scale
		float spriteScale = (zoomLevel <= 1.0f) ? 1.0f : zoomLevel;
		if (currentSprite.get())
		{
			currentSprite.get()->setScale(sf::Vector2f(spriteScale, spriteScale));
		}

		// Center the image or use saved position
		if (forceReset || !hasCustomPosition)
		{
			centerImage();
			savedImageOffset = sf::Vector2f(0, 0);
			hasCustomPosition = false;
		}
		else
		{
			// Apply saved offset from center
			sf::Vector2u windowSizeU = window.getSize();
			sf::Vector2f windowCenter(static_cast<float>(windowSizeU.x) / 2.0f,
				static_cast<float>(windowSizeU.y) / 2.0f);

			if (currentSprite.get())
			{
				sf::FloatRect spriteBounds = currentSprite.get()->getGlobalBounds();
				sf::Vector2f newImagePos = windowCenter - sf::Vector2f(spriteBounds.size.x / 2.0f,
					spriteBounds.size.y / 2.0f) + savedImageOffset;

				imagePosition = newImagePos;
				currentSprite.get()->setPosition(imagePosition);
			}
		}

		scrollOffset = 0;
		updateStatusText();
		updateDetailedInfo();
	}

	void nextImage() {
		NavigationHelper::executeIfNavigationAllowed(navLock, [this]() {
			if (currentImages.empty()) return;

			int nextIndex = currentImageIndex + 1;

			if (nextIndex >= currentImages.size())
			{
				nextFolder();
				return;
			}

			sf::Vector2u nextImageSize = getImageDimensions(nextIndex);
			if (nextImageSize.x > 0 && nextImageSize.y > 0)
			{
				if (sizeMismatchHandler.wouldNextImageNeedReset(nextImageSize))
				{
					resetZoomAndPosition();
				}
			}

			currentImageIndex = nextIndex;

			if (isCurrentlyInArchive)
			{
				archiveHandler.clearCache(currentImageIndex - 1);
			}

			loadCurrentImage();
		});
	}

	void previousImage() {
		NavigationHelper::executeIfNavigationAllowed(navLock, [this]() {
			if (currentImages.empty()) return;

			int prevIndex = currentImageIndex - 1;

			if (prevIndex < 0)
			{
				previousFolder();
				if (!currentImages.empty())
				{
					currentImageIndex = currentImages.size() - 1;
					loadCurrentImage();
				}
				return;
			}

			sf::Vector2u prevImageSize = getImageDimensions(prevIndex);
			if (prevImageSize.x > 0 && prevImageSize.y > 0)
			{
				if (sizeMismatchHandler.wouldNextImageNeedReset(prevImageSize))
				{
					resetZoomAndPosition();
				}
			}

			currentImageIndex = prevIndex;

			if (isCurrentlyInArchive)
			{
				archiveHandler.clearCache(currentImageIndex + 1);
			}

			loadCurrentImage();
		});
	}

	void nextFolder() {
		NavigationHelper::executeIfNavigationAllowed(navLock, [this]() {
			if (folders.empty()) return;

			if (folderLoadingFuture.valid())
			{
				folderLoadingFuture.wait();
			}

			if (isCurrentlyInArchive)
			{
				archiveHandler.closeArchive();
				isCurrentlyInArchive = false;
				currentArchivePath = L"";
			}

			int originalIndex = currentFolderIndex;
			do
			{
				currentFolderIndex++;
				if (currentFolderIndex >= folders.size())
				{
					currentFolderIndex = 0;
				}

				try
				{
					loadImagesFromFolder(folders[currentFolderIndex]);
					currentImageIndex = 0;
					if (!currentImages.empty() && loadCurrentImage())
					{
						updateNavigationButtons(); // Update button states
						return; // Successfully loaded next folder
					}
				} catch (...)
				{
					// Skip this folder and continue to next
				}

			} while (currentFolderIndex != originalIndex);

			// If we get here, no working folders found
			LockedMessageBox::showWarning(L"No more working folders found.", L"Navigation Warning");
		});
	}

	void previousFolder() {
		NavigationHelper::executeIfNavigationAllowed(navLock, [this]() {
			if (folders.empty()) return;

			if (folderLoadingFuture.valid())
			{
				folderLoadingFuture.wait();
			}

			if (isCurrentlyInArchive)
			{
				archiveHandler.closeArchive();
				isCurrentlyInArchive = false;
				currentArchivePath = L"";
			}

			int originalIndex = currentFolderIndex;
			do
			{
				currentFolderIndex--;
				if (currentFolderIndex < 0)
				{
					currentFolderIndex = folders.size() - 1;
				}

				try
				{
					loadImagesFromFolder(folders[currentFolderIndex]);
					currentImageIndex = 0;
					if (!currentImages.empty() && loadCurrentImage())
					{
						updateNavigationButtons(); // Update button states
						return; // Successfully loaded next folder
					}
				} catch (...)
				{
					// Skip this folder and continue to next
				}

			} while (currentFolderIndex != originalIndex);

			// If we get here, no working folders found
			LockedMessageBox::showWarning(L"No more working folders found.", L"Navigation Warning");
		});
	}

	void handleInput() {
		// Don't process input if message box is active
		if (LockedMessageBox::isActive())
		{
			return;
		}

		// Handle window resize
		sf::Vector2u currentSize = window.getSize();
		if (currentSize != lastWindowSize)
		{
			handleWindowResize(currentSize);
			lastWindowSize = currentSize;
		}

		window.handleEvents(
			[&](const sf::Event::Closed&) {
				if (!LockedMessageBox::isActive())
				{
					window.close();
				}
			},
			[&](const sf::Event::KeyPressed& kp) {
				if (LockedMessageBox::isActive()) return;

				switch (kp.code)
				{
				case sf::Keyboard::Key::Up:
				case sf::Keyboard::Key::W:
					if (navLock.isNavigationAllowed()) handleScroll(sf::Vector2f(0, -50));
					break;
				case sf::Keyboard::Key::Down:
				case sf::Keyboard::Key::S:
					if (navLock.isNavigationAllowed()) handleScroll(sf::Vector2f(0, 50));
					break;
				case sf::Keyboard::Key::Left:
				case sf::Keyboard::Key::A:
					if (navLock.isNavigationAllowed()) handleScroll(sf::Vector2f(-50, 0));
					break;
				case sf::Keyboard::Key::Right:
				case sf::Keyboard::Key::D:
					if (navLock.isNavigationAllowed()) handleScroll(sf::Vector2f(50, 0));
					break;
				case sf::Keyboard::Key::Tab:
					nextFolder(); // nextFolder checks navLock internally
					break;
				case sf::Keyboard::Key::F:
					if (navLock.isNavigationAllowed()) fitToWindow(true);
					break;
				case sf::Keyboard::Key::C:
					if (navLock.isNavigationAllowed())
					{
						centerImage();
						updateSavedOffset();
					}
					break;
				case sf::Keyboard::Key::H:
					showUI = !showUI; // Allow UI toggle even when locked
					break;
				case sf::Keyboard::Key::I:
					buttonManager.toggleButton(ButtonID::INFO_BUTTON);
					saveCurrentSession();
					break;
				case sf::Keyboard::Key::R:
					if (navLock.isNavigationAllowed()) selectNewMangaFolder();
					break;
				case sf::Keyboard::Key::Q:
					if (navLock.isNavigationAllowed()) toggleSmoothing();
					break;
				case sf::Keyboard::Key::F11:
					toggleFullscreen();  // Changed from toggleMaximize()
					saveCurrentSession(); // Save state change
					break;
				case sf::Keyboard::Key::F10:
					if (!isCurrentlyFullscreen)
					{ // Only allow maximize when not fullscreen
						toggleMaximize();
						saveCurrentSession();
					}
					break;
				}
			},
			[&](const sf::Event::MouseWheelScrolled& mwhl) {
				if (LockedMessageBox::isActive()) return;

				if (mwhl.wheel == sf::Mouse::Wheel::Vertical)
				{
					if (sf::Keyboard::isKeyPressed(sf::Keyboard::Key::LControl) ||
						sf::Keyboard::isKeyPressed(sf::Keyboard::Key::RControl))
					{
						// Zoom with Ctrl + Mouse Wheel
						if (navLock.isNavigationAllowed()) handleZoom(mwhl.delta);
					}
					else
					{
						// Navigate images with Mouse Wheel
						if (mwhl.delta > 0)
						{
							previousImage(); // previousImage checks navLock internally
						}
						else
						{
							nextImage(); // nextImage checks navLock internally
						}
					}
				}
			},
			[&](const sf::Event::MouseButtonPressed& mb) {
				if (LockedMessageBox::isActive()) return;

				if (mb.button == sf::Mouse::Button::Middle)
				{
					if (navLock.isNavigationAllowed()) fitToWindow(true);
				}
				else if (mb.button == sf::Mouse::Button::Left)
				{
					sf::Vector2f mousePos = window.mapPixelToCoords(mb.position);
					ButtonID clickedButton = buttonManager.checkButtonClick(mousePos);

					if (clickedButton != ButtonID::COUNT)
					{
						handleButtonClick(clickedButton);
					}
				}
			},
			[&](const sf::Event::MouseButtonReleased& mb) {
				if (LockedMessageBox::isActive()) return;
				// Handle mouse drag for panning
			},
			[&](const sf::Event::Resized& resize) {
				if (LockedMessageBox::isActive()) return;
					handleWindowResize(sf::Vector2u(resize.size.x, resize.size.y));
				saveCurrentSession(); // Save window size changes
			},
			[&](const sf::Event::FocusLost&) {
				if (LockedMessageBox::isActive()) return;
			},
			[&](const sf::Event::FocusGained&) {
				if (LockedMessageBox::isActive()) return;
			}
		);
	}

public: //rendering
	void drawLoadingOverlay(sf::RenderWindow& window) {
		if (!isLoadingFolder) return;

		// Semi-transparent overlay
		sf::RectangleShape overlay;
		overlay.setSize(sf::Vector2f(static_cast<float>(window.getSize().x),
			static_cast<float>(window.getSize().y)));
		overlay.setFillColor(sf::Color(0, 0, 0, 150));
		window.draw(overlay);

		// Loading text background
		sf::RectangleShape loadingBg;
		loadingBg.setSize(sf::Vector2f(400.0f, 60.0f));
		loadingBg.setPosition(sf::Vector2f(
			(static_cast<float>(window.getSize().x) - 400.0f) / 2.0f,
			(static_cast<float>(window.getSize().y) - 60.0f) / 2.0f
		));
		loadingBg.setFillColor(sf::Color(50, 50, 50, 200));
		loadingBg.setOutlineThickness(2);
		loadingBg.setOutlineColor(sf::Color::White);
		window.draw(loadingBg);

		// Center the loading text
		loadingText.get()->setPosition(sf::Vector2f(
			loadingBg.getPosition().x + 20.0f,
			loadingBg.getPosition().y + 20.0f
		));
		window.draw(*loadingText.get());
	}

	void render() {
		window.clear(sf::Color::Black);

		// Draw current image
		if (currentSprite.get()->getTexture().getSize().x > 0)
		{
			window.draw(*currentSprite.get());
		}

		// Draw UI
		if (showUI)
		{
			// Semi-transparent background for status text
			sf::RectangleShape statusBg;
			statusBg.setSize(sf::Vector2f(450.0f, 100.0f));
			statusBg.setPosition(sf::Vector2f(5.0f, 5.0f));
			statusBg.setFillColor(sf::Color(0, 0, 0, 150));
			window.draw(statusBg);

			window.draw(*statusText.get());

			// Draw detailed info if visible
			if (buttonManager.isButtonToggled(ButtonID::INFO_BUTTON))
			{
				sf::RectangleShape detailedBg;
				sf::FloatRect textBounds = detailedInfoText.get()->getLocalBounds();

				detailedBg.setSize({ textBounds.size.x + 20.f, textBounds.size.y + 20.f });
				detailedBg.setPosition(sf::Vector2f(5.0f, 115.0f));
				detailedBg.setFillColor(sf::Color(0, 0, 0, 180));
				detailedBg.setOutlineThickness(2);
				detailedBg.setOutlineColor(sf::Color::Cyan);
				window.draw(detailedBg);

				detailedInfoText.get()->setPosition(sf::Vector2f(detailedBg.getPosition().x + 10.f,
					detailedBg.getPosition().y + 10.f));

				window.draw(*detailedInfoText.get());
			}

			if (showHelpText && buttonManager.isButtonToggled(ButtonID::HELP_BUTTON))
			{
				// Optional: Add semi-transparent background for help text
				sf::FloatRect helpBounds = helpText.get()->getLocalBounds();
				sf::RectangleShape helpBg;
				helpBg.setSize({ helpBounds.size.x + 20.f, helpBounds.size.y + 20.f });
				helpBg.setPosition(sf::Vector2f(helpText.get()->getPosition().x - 10.f,
					helpText.get()->getPosition().y - 10.f));
				helpBg.setFillColor(sf::Color(0, 0, 0, 150));
				window.draw(helpBg);

				window.draw(*helpText.get());
			}
		}

		// SHOW NAVIGATION LOCK INDICATOR when locked
		if (navLock.isNavigationLocked())
		{
			sf::RectangleShape lockIndicator;
			lockIndicator.setSize(sf::Vector2f(350.0f, 70.0f));
			lockIndicator.setPosition(sf::Vector2f(
				(static_cast<float>(window.getSize().x) - 350.0f) / 2.0f,
				static_cast<float>(window.getSize().y) - 100.0f
			));
			lockIndicator.setFillColor(sf::Color(255, 165, 0, 220)); // Orange
			lockIndicator.setOutlineThickness(3);
			lockIndicator.setOutlineColor(sf::Color::White);
			window.draw(lockIndicator);

			sf_text_wrapper lockText;
			lockText.initialize(*font.get(), 18u);
			std::string message = "NAVIGATION LOCKED\n" + navLock.getCurrentOperation() + "...";
			lockText.get()->setString(message);
			lockText.get()->setFillColor(sf::Color::White);
			lockText.get()->setPosition(sf::Vector2f(
				lockIndicator.getPosition().x + 20.0f,
				lockIndicator.getPosition().y + 15.0f
			));
			window.draw(*lockText.get());
		}

		// Draw loading overlay if loading folder
		if (isLoadingFolder)
		{
			updateLoadingProgress();
			drawLoadingOverlay(window);
		}

		// Draw loading overlay if loading folder
		buttonManager.drawAll(window);

		window.display();
	}

	void forceUnlockNavigation() {
		navLock.forceUnlock();
	}

	bool isNavigationCurrentlyLocked() {
		return navLock.isNavigationLocked();
	}

public: //main aplication function

	bool isInitialized() const {
		return window.isOpen();
	}

	std::wstring browseForFolder() {
		BROWSEINFOW bi = {};
		wchar_t path[MAX_PATH];

		bi.lpszTitle = L"Select Manga Root Folder";
		bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;

		LPITEMIDLIST pidl = SHBrowseForFolderW(&bi);
		if (pidl != nullptr)
		{
			if (SHGetPathFromIDListW(pidl, path))
			{
				CoTaskMemFree(pidl);
				return std::wstring(path);
			}

			CoTaskMemFree(pidl);
		}

		return L"";
	}

	void run() {
		while (window.isOpen())
		{
			handleInput();
			render();
		}
	}

};

CommandLineOptions parseCommandLine(int argc, char* argv[]) {
	CLI::App app{ "Simple Manga Reader - A manga/comic archive viewer" };
	app.set_version_flag("--version,-v", "1.0.0");

	CommandLineOptions options;

	// Path-related options
	app.add_flag("--enable-long-paths", options.enableLongPaths,
		"Attempt to enable Windows long path support (requires admin)");

	app.add_flag("--show-path-info", options.showPathInfo,
		"Display current path length settings and exit");

	// Configuration options
	app.add_option("--config,-c", options.configFile,
		"Specify custom configuration file path")
		->check(CLI::ExistingFile);

	app.add_option("--manga-folder,-m", options.mangaFolder,
		"Start with specific manga folder")
		->check(CLI::ExistingDirectory);

	// Debug options
	app.add_flag("--verbose", options.verbose,
		"Enable verbose logging output");

	try
	{
		app.parse(argc, argv);
	} catch (const CLI::ParseError& e)
	{
		// CLI11 handles help and error messages automatically
		std::exit(app.exit(e));
	}

	return options;
}

std::vector<std::string> parseWindowsCommandLine(LPSTR lpCmdLine) {
	std::vector<std::string> args;

	// Add program name as argv[0]
	wchar_t exePath[MAX_PATH];
	GetModuleFileNameW(NULL, exePath, MAX_PATH);
	args.push_back(UnicodeUtils::wstringToString(std::wstring(exePath)));

	// Parse command line arguments
	if (lpCmdLine && strlen(lpCmdLine) > 0)
	{
		std::string cmdLine(lpCmdLine);
		std::istringstream iss(cmdLine);
		std::string arg;

		// Simple space-separated parsing (CLI11 can handle more complex cases)
		while (iss >> arg)
		{
			args.push_back(arg);
		}
	}

	return args;
}

// Convert vector<string> to char* argv[] for CLI11
std::vector<char*> convertToArgv(const std::vector<std::string>& args) {
	std::vector<char*> argv;
	argv.reserve(args.size());

	for (const auto& arg : args)
	{
		argv.push_back(const_cast<char*>(arg.c_str()));
	}

	return argv;
}

// Windows entry point for GUI application (no console window)
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
	try
	{
		// Parse command line arguments
		auto argStrings = parseWindowsCommandLine(lpCmdLine);
		auto argv = convertToArgv(argStrings);
		int argc = static_cast<int>(argv.size());

		CommandLineOptions options = parseCommandLine(argc, argv.data());

		// Handle command line options before starting GUI
		if (options.showPathInfo)
		{
			PathLimitChecker::showPathInfoConsole();
			return 0;
		}

		if (options.enableLongPaths)
		{
			PathLimitChecker::handleEnableLongPaths();
			// Continue to start application after attempting to enable
		}

		// Create reader with validation
		MangaReader reader(options);

		// Check if reader was initialized successfully
		if (!reader.isInitialized())
		{
			return 1; // Exit gracefully if initialization failed
		}

		reader.run();

	} catch (const CLI::ParseError& e)
	{
		// CLI11 parsing error - this is expected for --help, etc.
		return 1;
	} catch (const std::exception& e)
	{
		// Show error in message box (no console in Windows app)
		std::wstring errorMsg = L"Application Error: " + UnicodeUtils::stringToWstring(e.what());
		LockedMessageBox::showError(errorMsg, L"Manga Reader Error");
		return 1;
	} catch (...)
	{
		LockedMessageBox::showError(L"Unknown error occurred during startup.", L"Manga Reader Error");
		return 1;
	}

	return 0;
}