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

// Define SFML_STATIC if not already defined (for static linking)
#ifndef SFML_STATIC
#define SFML_STATIC
#endif

#include <archive.h>
#include <archive_entry.h>

#include <webp/decode.h>

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

// Info Button Class
class InfoButton {
private:
	sf::RectangleShape button;
	sf::CircleShape infoIcon;
	sf_text_wrapper buttonText;
	bool isInfoVisible;
	sf::Vector2f buttonPosition;  // Store position for easier access
	float buttonSize;             // Store size for easier access

public:
	InfoButton() : button(), infoIcon(), buttonText(), isInfoVisible(false), buttonPosition(), buttonSize(30.0f) { }

	void initialize(const sf::Font& font, float x, float y, float size = 30.0f) {
		// Setup button (small circular button)
		button.setSize(sf::Vector2f(size, size));
		button.setPosition(sf::Vector2f(x, y));
		button.setFillColor(sf::Color(70, 130, 180, 200)); // Semi-transparent steel blue
		button.setOutlineThickness(1);
		button.setOutlineColor(sf::Color::White);

		// Setup info icon (circle background)
		infoIcon.setRadius(size / 2 - 3);
		infoIcon.setPosition(sf::Vector2f(x + 3, y + 3));
		infoIcon.setFillColor(sf::Color(255, 255, 255, 180));

		// Setup "i" text on button
		buttonText.initialize(font, static_cast<unsigned int>(size - 8));
		buttonText.get()->setString("i");
		buttonText.get()->setFillColor(sf::Color(70, 130, 180));
		buttonText.get()->setPosition(sf::Vector2f(x + size / 2 - 4, y + size / 2 - 12));
		buttonText.get()->setStyle(sf::Text::Bold);  // Make text bold for better visibility

		// Center the text better
		sf::FloatRect textBounds = buttonText.get()->getLocalBounds();
		buttonText.get()->setPosition(sf::Vector2f(
			x + (size - textBounds.size.x) / 2 - textBounds.position.x,
			y + (size - textBounds.size.y) / 2 - textBounds.position.y)
		);
	}

	// Update button position (useful for window resizing)
	void updatePosition(float x, float y) {
		buttonPosition = sf::Vector2f(x, y);
		button.setPosition(buttonPosition);
		infoIcon.setPosition(sf::Vector2f(x + 4, y + 4));

		// Recenter text
		sf::FloatRect textBounds = buttonText.get()->getLocalBounds();
		buttonText.get()->setPosition(sf::Vector2f(
			x + (buttonSize - textBounds.size.x) / 2 - textBounds.position.x,
			y + (buttonSize - textBounds.size.y) / 2 - textBounds.position.y
		));
	}

	bool isClickedExpanded(sf::Vector2f mousePos, float expandBy = 5.0f) {
		sf::FloatRect expandedBounds = button.getGlobalBounds();
		expandedBounds.position.x -= expandBy;
		expandedBounds.position.y -= expandBy;
		expandedBounds.size.x += expandBy * 2;
		expandedBounds.size.y += expandBy * 2;

		return expandedBounds.contains(mousePos);
	}

	bool isClicked(sf::Vector2f mousePos) {
		// Use button's global bounds for click detection
		sf::FloatRect bounds = button.getGlobalBounds();

		// Add some debug output (remove in release)
//#ifdef _DEBUG
//		static int debugClickCount = 0;
//		debugClickCount++;
//		if (debugClickCount % 10 == 0)
//		{  // Only print every 10th click to avoid spam
//			std::cout << "Mouse pos: " << mousePos.x << ", " << mousePos.y << std::endl;
//			std::cout << "Button bounds: " << bounds.position.x << ", " << bounds.position.y
//				<< " to " << (bounds.position.x + bounds.size.x)
//				<< ", " << (bounds.position.y + bounds.size.y) << std::endl;
//			std::cout << "Contains: " << (bounds.contains(mousePos) ? "YES" : "NO") << std::endl;
//		}
//#endif

		return bounds.contains(mousePos);
	}

	void toggleInfo() {
		isInfoVisible = !isInfoVisible;

		// Visual feedback when toggled
		if (isInfoVisible)
		{
			button.setFillColor(sf::Color(100, 160, 210, 220)); // Brighter when active
			button.setOutlineColor(sf::Color::Cyan);
		}
		else
		{
			button.setFillColor(sf::Color(70, 130, 180, 200)); // Normal color
			button.setOutlineColor(sf::Color::White);
		}
	}

	bool getInfoVisible() const {
		return isInfoVisible;
	}

	sf::FloatRect getBounds() const {
		return button.getGlobalBounds();
	}

	void draw(sf::RenderWindow& window) {
		window.draw(button);
		window.draw(infoIcon);
		window.draw(*buttonText.get());
	}
};

// Archive Entry Structure
struct ArchiveEntry {
	std::string name;
	size_t size;
	int index; // Index in archive
};

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

// Updated ArchiveHandler class with RAR support
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
				showCriticalError("File Check", "Archive file does not exist: " + archivePath);
				return false;
			}

			// Check file size
			auto fileSize = std::filesystem::file_size(path);
			if (fileSize == 0)
			{
				showCriticalError("File Check", "Archive file is empty");
				return false;
			}

			archive = archive_read_new();
			if (!archive)
			{
				showCriticalError("Archive Creation", "Failed to create archive object");
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
				showCriticalError("Archive Opening", errorMsg);
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
			showCriticalError("Filesystem Error", e.what());
			closeArchiveInternal();
			return false;
		} catch (const std::exception& e)
		{
			showCriticalError("Exception in openArchive", e.what());
			closeArchiveInternal();
			return false;
		} catch (...)
		{
			showCriticalError("Unknown Exception", "Unknown exception occurred in openArchive");
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
			showCriticalError("clearCache", e.what());
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
				showCriticalError("Parameter Validation", error);
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
				showCorruptionError(entryIndex, imageEntries[entryIndex].name);
				return false;
			}

			// Return cached data
			if (entryIndex < cachedImages.size() && !cachedImages[entryIndex].empty())
			{
				buffer = cachedImages[entryIndex];
				return true;
			}

			showCriticalError("Cache Error", "Cache is empty after successful extraction for entry: " + std::to_string(entryIndex));
			return false;

		} catch (const std::bad_alloc& e)
		{
			corruptedEntries.insert(entryIndex);
			showMemoryError("extractImageToMemory", 0);
			return false;
		} catch (const std::exception& e)
		{
			corruptedEntries.insert(entryIndex);
			showCriticalError("extractImageToMemory Exception",
				"Entry: " + std::to_string(entryIndex) + " - " + e.what());
			return false;
		} catch (...)
		{
			corruptedEntries.insert(entryIndex);
			showCriticalError("extractImageToMemory Unknown Exception",
				"Unknown exception for entry: " + std::to_string(entryIndex));
			return false;
		}
	}

	void showCriticalError(const std::string& operation, const std::string& error) {
		std::wstring message = L"CRITICAL ARCHIVE ERROR\n\n";
		message += L"Archive: " + archivePathW + L"\n";
		message += L"Operation: " + UnicodeUtils::stringToWstring(operation) + L"\n";
		message += L"Error: " + UnicodeUtils::stringToWstring(error) + L"\n\n";
		message += L"This archive may be corrupted or incompatible.";

		LockedMessageBox::showError(message, L"Archive Error");
	}

	void showNonCriticalError(const std::string& operation, const std::string& error) {
		std::wstring message = L"ARCHIVE ERROR (Skipping):\n\n";
		message += L"Archive: " + archivePathW + L"\n";
		message += L"Operation: " + UnicodeUtils::stringToWstring(operation) + L"\n";
		message += L"Error: " + UnicodeUtils::stringToWstring(error) + L"\n\n";
		message += L"This archive will be skipped and the next one will be tried.";

		LockedMessageBox::showWarning(message, L"Archive Skipped");
	}

	void showMemoryError(const std::string& operation, size_t requestedSize) {
		std::wstring message = L"MEMORY ERROR\n\n";
		message += L"Archive: " + archivePathW + L"\n";
		message += L"Operation: " + UnicodeUtils::stringToWstring(operation) + L"\n";
		message += L"Requested Size: " + std::to_wstring(requestedSize / 1024 / 1024) + L" MB\n\n";

		// Get current memory info
		MEMORYSTATUSEX memInfo;
		memInfo.dwLength = sizeof(MEMORYSTATUSEX);
		if (GlobalMemoryStatusEx(&memInfo))
		{
			message += L"Available Memory: " + std::to_wstring(memInfo.ullAvailPhys / 1024 / 1024) + L" MB\n";
			message += L"Total Memory: " + std::to_wstring(memInfo.ullTotalPhys / 1024 / 1024) + L" MB\n\n";
		}

		message += L"The image is too large or system is low on memory.\n";
		message += L"Try closing other applications or skip this image.";

		LockedMessageBox::showWarning(message, L"Memory Error");
	}

	void showCorruptionError(int entryIndex, const std::string& fileName) {
		std::wstring message = L"IMAGE CORRUPTION DETECTED\n\n";
		message += L"Archive: " + archivePathW + L"\n";
		message += L"Image: " + UnicodeUtils::stringToWstring(fileName) + L"\n";
		message += L"Entry Index: " + std::to_wstring(entryIndex) + L"\n\n";
		message += L"This image appears to be corrupted and will be skipped.";

		LockedMessageBox::showWarning(message, L"Image Corruption");
	}

	bool isSafeToAllocate(size_t requestedSize) {
		const size_t MAX_SINGLE_ALLOCATION = 200 * 1024 * 1024; // 200MB max per image
		const size_t MIN_FREE_MEMORY = 500 * 1024 * 1024; // Keep 500MB free

		if (requestedSize > MAX_SINGLE_ALLOCATION)
		{
			showMemoryError("Size Check", requestedSize);
			return false;
		}

		MEMORYSTATUSEX memInfo;
		memInfo.dwLength = sizeof(MEMORYSTATUSEX);
		if (GlobalMemoryStatusEx(&memInfo))
		{
			if (memInfo.ullAvailPhys < (requestedSize + MIN_FREE_MEMORY))
			{
				showMemoryError("Memory Check", requestedSize);
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
			showMemoryError("Vector Allocation", size);
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
			showCriticalError("loadImageEntries Exception", e.what());
			return false;
		} catch (...)
		{
			showCriticalError("loadImageEntries", "Unknown exception occurred");
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

		// Get just the archive filename without path
		std::filesystem::path archivePath(archiveName);
		std::string archiveFilename = archivePath.filename().string();
		std::string archiveDir = archivePath.parent_path().string();

		// Calculate potential path lengths
		size_t archivePathLength = archiveName.length();
		size_t basePathLength = archiveDir.length();

		// Conservative limits for Windows compatibility
		const size_t MAX_TOTAL_PATH = 240;  // Leave buffer for Windows MAX_PATH (260)
		const size_t MAX_SINGLE_COMPONENT = 80;  // Max length for any single folder/file name
		const size_t MAX_ESTIMATED_INTERNAL_PATH = 120; // Estimated max internal folder + filename

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
		// Base path + archive name + estimated internal folders + filename
		size_t estimatedMaxPath = basePathLength + archiveFilename.length() + MAX_ESTIMATED_INTERNAL_PATH;
		if (estimatedMaxPath > MAX_TOTAL_PATH)
		{
			estimatedOverflow = true;
		}

		if (pathTooLong || componentTooLong || estimatedOverflow)
		{
			std::wstring message = L"ARCHIVE SKIPPED - PATH LENGTH ISSUE:\n\n";
			message += L"Archive: " + UnicodeUtils::stringToWstring(archiveFilename) + L"\n\n";

			if (pathTooLong)
			{
				message += L"• Archive path too long: " + std::to_wstring(archivePathLength) + L" chars (max " + std::to_wstring(MAX_TOTAL_PATH) + L")\n";
			}
			if (componentTooLong)
			{
				message += L"• Archive filename too long: " + std::to_wstring(archiveFilename.length()) + L" chars (max " + std::to_wstring(MAX_SINGLE_COMPONENT) + L")\n";
			}
			if (estimatedOverflow)
			{
				message += L"• Estimated total path would exceed Windows limit\n";
				message += L"  (Base: " + std::to_wstring(basePathLength) + L" + Archive: " + std::to_wstring(archiveFilename.length()) + L" + Internal: ~" + std::to_wstring(MAX_ESTIMATED_INTERNAL_PATH) + L" = " + std::to_wstring(estimatedMaxPath) + L")\n";
			}

			message += L"\nThis archive will be automatically skipped.\n";
			message += L"Moving to next archive...";

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
				showCriticalError("Archive Reopen", "Failed to create new archive object for extraction");
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
				showCriticalError("Archive Reopen", errorMsg);
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
							showMemoryError("Image Too Large", static_cast<size_t>(size));
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
								showCriticalError("Archive Read Error", error);
								cachedImages[targetIndex].clear();
							}
							else
							{
								std::string error = "Partial read for: " + currentPath +
									". Expected: " + std::to_string(size) +
									", Got: " + std::to_string(bytesRead);
								showCriticalError("Partial Read", error);
								cachedImages[targetIndex].clear();
							}
						} catch (const std::bad_alloc& e)
						{
							showMemoryError("Allocation Failed", static_cast<size_t>(size));
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
			showCriticalError("extractAndCacheImageInternal Exception", e.what());
			return false;
		} catch (...)
		{
			showCriticalError("extractAndCacheImageInternal", "Unknown exception occurred");
			return false;
		}
	}};

struct FoldersIdent {
	std::wstring dir;
	bool isArchieve;

	bool operator<(const FoldersIdent& other) const {
		return dir < other.dir;  // std::wstring already has operator<
	}
};

class MangaReader {
private:
	sf::RenderWindow window;

	sf::Texture originalTexture;      // Store original high-res texture
	sf::Texture scaledTexture;        // Store scaled texture for display

	sf_Sprite_wrapper currentSprite;
	sf_font_wrapper font;
	sf_text_wrapper statusText;
	sf_text_wrapper helpText;
	sf_text_wrapper detailedInfoText;  // New detailed info text
	InfoButton infoButton;             // New info button

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

public: //constructor and destructor

	MangaReader() : window(sf::VideoMode(sf::Vector2u(1200, 800)), "Simple Manga Reader")
		, originalTexture()
		, scaledTexture()
		, currentSprite()
		, font()
		, statusText()
		, helpText()
		, detailedInfoText()
		, infoButton()
		, folders()
		, currentImages()
		, currentFolderIndex(0)
		, currentImageIndex(0)
		, scrollOffset(0.0f)
		, zoomLevel(1.0f)
		, imagePosition()
		// Scaling support
		, useSmoothing(true)
		, lastZoomLevel()              // Track zoom changes for rescaling
		, lastWindowSize()     // Track window size changes
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

		// Progress display
		, loadingText()

		// Enhanced zoom and positioning management
		, savedZoomLevel(1.0f)          // Remember zoom level across images in same folder
		, savedImageOffset() // Remember manual positioning offset
		, hasCustomZoom()           // Track if user has set custom zoom
		, hasCustomPosition()        // Track if user has manually positioned image
		, currentView()          // Manage view properly
		, sizeMismatchHandler()
		, navLock()
	{

		window.setFramerateLimit(60);

		// Get the native window handle and set it for our message box class
		HWND hwnd = window.getNativeHandle();
		LockedMessageBox::setMainWindow(hwnd);

		// Initialize view
		sf::Vector2u windowSize = window.getSize();
		currentView.setSize({ static_cast<float>(windowSize.x), static_cast<float>(windowSize.y) });
		currentView.setCenter({ static_cast<float>(windowSize.x) / 2.0f, static_cast<float>(windowSize.y) / 2.0f });
		window.setView(currentView);

		// Initialize COM for folder dialog
		CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
		// Set console to UTF-8 for proper Unicode handling
		SetConsoleOutputCP(CP_UTF8);
		SetConsoleCP(CP_UTF8);

		setupUI();

		loadingText.initialize(*font.get(), 18u);
		loadingText.get()->setFillColor(sf::Color::White);

		// Browse for manga root folder on startup
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

		bool foundWorkingFolder = false;
		for (int i = 0; i < folders.size() && !foundWorkingFolder; i++) {
			currentFolderIndex = i;
			try
			{
				loadImagesFromFolder(folders[currentFolderIndex]);
				if (!currentImages.empty() && loadCurrentImage())
				{
					foundWorkingFolder = true;
				}
			} catch (...)
			{
				// Skip this folder and try the next one
				continue;
			}
		}

		if (!foundWorkingFolder) {
			LockedMessageBox::showError(L"No working manga folders or archives found.", L"No Working Content");
			window.close();
			return;
		}
	}

	~MangaReader() {
		//Cleanup COM
		CoUninitialize();
	}

public: //helpers

	bool isWebPFile(const std::string& filename) {
		std::filesystem::path path(filename);
		std::string extension = path.extension().string();
		std::transform(extension.begin(), extension.end(), extension.begin(), ::tolower);
		return extension == ".webp";
	}

	std::string GetFilenameForArchived(std::wstring& str) {
		std::string filename = UnicodeUtils::wstringToString(str);
		size_t hashPos = filename.find('#');
		if (hashPos != std::string::npos)
		{
			filename = filename.substr(hashPos + 1);
		}

		// Extract just the filename from full path (handle folders)
		std::filesystem::path p(filename);
		return p.filename().string();
	}

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

	// Get file size in a readable format
	std::string getFileSizeString(const std::wstring& filePath) {
		try
		{
			auto fileSize = std::filesystem::file_size(filePath);

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
		} catch (...)
		{
			return "Unknown";
		}
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

		try
		{
			if (isCurrentlyInArchive)
			{
				// For archives, get size from archive entry info
				const auto& entries = archiveHandler.getImageEntries();
				if (imageIndex < entries.size())
				{
					// Try to extract and get dimensions quickly
					std::vector<uint8_t> rawData;
					if (archiveHandler.extractImageToMemory(imageIndex, rawData))
					{
						sf::Image tempImage;
						std::string filename = GetFilenameForArchived(currentImages[imageIndex]);

						if (isWebPFile(filename))
						{
							if (loadWebPFromMemory(rawData, tempImage))
							{
								return tempImage.getSize();
							}
						}
						else
						{
							if (tempImage.loadFromMemory(rawData.data(), rawData.size()))
							{
								return tempImage.getSize();
							}
						}
					}
				}
			}
			else
			{
				// For regular files, use SFML to get dimensions quickly
				sf::Image tempImage;
				std::string filename = UnicodeUtils::wstringToString(currentImages[imageIndex]);

				if (isWebPFile(filename))
				{
					if (loadWebPFromFile(currentImages[imageIndex], tempImage))
					{
						return tempImage.getSize();
					}
				}
				else
				{
					if (tempImage.loadFromFile(currentImages[imageIndex]))
					{
						return tempImage.getSize();
					}
				}
			}
		} catch (...)
		{
			// Return 0,0 on any error
		}

		return sf::Vector2u(0, 0);
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

		return false; // No working folders found
	}

	bool loadWebPFromMemory(const std::vector<uint8_t>& data, sf::Image& image) {
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

	bool loadWebPFromFile(const std::wstring& filePath, sf::Image& image, int sizeLimit = 100 * 1024 * 1024) {

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

		sf::Image image;
		bool loaded = false;

		try
		{
			if (isCurrentlyInArchive)
			{
				std::vector<uint8_t> rawData;
				if (archiveHandler.extractImageToMemory(index, rawData))
				{
					loaded = isWebPFile(GetFilenameForArchived(currentImages[index])) ? loadWebPFromMemory(rawData, image) : image.loadFromMemory(rawData.data(), rawData.size());
				}
			}
			else
			{
				std::string filename = UnicodeUtils::wstringToString(currentImages[index]);
				loaded = isWebPFile(filename) ? loadWebPFromFile(currentImages[index], image) : image.loadFromFile(currentImages[index]);
			}

			if (loaded)
			{
				std::lock_guard<std::mutex> lock(loadingMutex);
				loadedImages[index].image = std::move(image);
				loadedImages[index].filename = UnicodeUtils::wstringToString(
					std::filesystem::path(currentImages[index]).filename().wstring());
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
		} catch (const std::exception& e)
		{
			// Skip failed images
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

			if (isCurrentlyInArchive)
			{
				std::vector<uint8_t> rawData;
				if (archiveHandler.extractImageToMemory(currentImageIndex, rawData))
				{
					std::string filename = GetFilenameForArchived(currentImages[currentImageIndex]);
					loaded = isWebPFile(filename) ? loadWebPFromMemory(rawData, imageData) : imageData.loadFromMemory(rawData.data(), rawData.size());
				}
			}
			else
			{
				std::string filename = UnicodeUtils::wstringToString(currentImages[currentImageIndex]);
				loaded = isWebPFile(filename) ? loadWebPFromFile(currentImages[currentImageIndex], imageData) : imageData.loadFromFile(currentImages[currentImageIndex]);
			}

			if (loaded)
			{
				setupTextureFromImage(imageData);
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
				return true;
			}
		}

		sf::Image imageData;
		bool loaded = false;

		if (isCurrentlyInArchive)
		{
			// Load from archive
			std::vector<uint8_t> rawData;
			if (archiveHandler.extractImageToMemory(currentImageIndex, rawData))
			{
				std::string filename = GetFilenameForArchived(currentImages[currentImageIndex]);
				loaded = isWebPFile(filename) ? loadWebPFromMemory(rawData, imageData) : imageData.loadFromMemory(rawData.data(), rawData.size());
			}

		}
		else
		{
			std::string filename = UnicodeUtils::wstringToString(currentImages[currentImageIndex]);
			loaded = isWebPFile(filename) ? loadWebPFromFile(currentImages[currentImageIndex], imageData) : imageData.loadFromFile(currentImages[currentImageIndex]);
		}

		if (loaded)
		{
			setupTextureFromImage(imageData);
			return true;
		}

		// Show error if image fails to load
		std::wstring imagePath = currentImages[currentImageIndex];
		std::wstring errorMsg = L"Failed to load image: " + imagePath;
		LockedMessageBox::showError(errorMsg, L"Image Loading Error");
		return false;
	}

public://setup

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
			"R: Select new manga folder"
			"Left Click Info Button: Toggle info"
		);

		// Setup detailed info text
		detailedInfoText.initialize(*font.get(), 14u);
		detailedInfoText.get()->setFillColor(sf::Color::Cyan);
		detailedInfoText.get()->setPosition(sf::Vector2f(10.0f, 120.0f)); // Below status text

		// Setup info button (top-right corner with some padding)
		updateInfoButtonPosition();
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

		// Update UI positions
		updateInfoButtonPosition();
		helpText.get()->setPosition(sf::Vector2f(10.0f, static_cast<float>(newSize.y) - 180.0f));

		// Refit image with current zoom preferences
		if (originalTexture.getSize().x > 0)
		{
			fitToWindow(false); // Don't force reset, maintain user preferences
		}

		lastWindowSize = newSize;
	}

public: //update funcs

	void updateInfoButtonPosition() {
		float buttonX = static_cast<float>(window.getSize().x) - 50.0f;  // More padding from edge
		float buttonY = 10.0f;

		static bool infoButtonInitialized = { false };

		if (!infoButtonInitialized)
		{
			infoButton.initialize(*this->font.get(), buttonX, buttonY);
			infoButtonInitialized = true;
		}

		infoButton.updatePosition(buttonX, buttonY);
	}

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
				if (currentImageIndex < entries.size())
				{
					size_t size = entries[currentImageIndex].size;
					if (size < 1024)
					{
						fileSize = std::to_string(size) + " B";
					}
					else if (size < 1024 * 1024)
					{
						fileSize = std::to_string(size / 1024) + " KB";
					}
					else
					{
						fileSize = std::to_string(size / (1024 * 1024)) + " MB";
					}
				}
			}
			else
			{
				fileName = UnicodeUtils::getFilenameOnly(imagePath);
				std::filesystem::path path(currentImages[currentImageIndex]);
				extension = UnicodeUtils::wstringToString(path.extension().wstring());
				fileSize = getFileSizeString(currentImages[currentImageIndex]);
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
		// CHECK if navigation is allowed
		if (!navLock.isNavigationAllowed())
		{
			return; // Silently ignore if locked
		}

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
	}

	void previousImage() {
		// CHECK if navigation is allowed
		if (!navLock.isNavigationAllowed())
		{
			return; // Silently ignore if locked
		}

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
	}

	void nextFolder() {
		// CHECK if navigation is allowed
		if (!navLock.isNavigationAllowed())
		{
			return; // Silently ignore if locked
		}

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
					return; // Successfully loaded next folder
				}
			} catch (...)
			{
				// Skip this folder and continue to next
			}

		} while (currentFolderIndex != originalIndex);

		// If we get here, no working folders found
		LockedMessageBox::showWarning(L"No more working folders found.", L"Navigation Warning");
	}

	void previousFolder() {
		// CHECK if navigation is allowed
		if (!navLock.isNavigationAllowed())
		{
			return; // Silently ignore if locked
		}

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
					return; // Successfully loaded next folder
				}
			} catch (...)
			{
				// Skip this folder and continue to next
			}

		} while (currentFolderIndex != originalIndex);

		// If we get here, no working folders found
		LockedMessageBox::showWarning(L"No more working folders found.", L"Navigation Warning");
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
					infoButton.toggleInfo(); // Allow info toggle even when locked
					break;
				case sf::Keyboard::Key::R:
					if (navLock.isNavigationAllowed()) selectNewMangaFolder();
					break;
				case sf::Keyboard::Key::Q:
					if (navLock.isNavigationAllowed()) toggleSmoothing();
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
					if (infoButton.isClicked(window.mapPixelToCoords(mb.position)))
					{
						infoButton.toggleInfo(); // Allow info toggle even when locked
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
			if (infoButton.getInfoVisible())
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

			window.draw(*helpText.get());
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

		// Always draw info button
		infoButton.draw(window);

		window.display();
	}

	void forceUnlockNavigation() {
		navLock.forceUnlock();
	}

	bool isNavigationCurrentlyLocked() {
		return navLock.isNavigationLocked();
	}

public: //main aplication function

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

// Windows entry point for GUI application (no console window)
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
	try
	{
		MangaReader reader;
		reader.run();
	} catch (const std::exception& e)
	{
		// Show error in message box (no console in Windows app)
		std::wstring errorMsg = L"Error: " + UnicodeUtils::stringToWstring(e.what());
		LockedMessageBox::showError(errorMsg, L"Manga Reader Error");
		return 1;
	}

	return 0;
}