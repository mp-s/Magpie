#pragma once
#include "WindowBase.h"

class CursorWindow : public WindowBaseT<CursorWindow> {
	using base_type = WindowBaseT<CursorWindow>;
	friend base_type;

public:
	bool Create() noexcept;

private:
	LRESULT _MessageHandler(UINT msg, WPARAM wParam, LPARAM lParam) noexcept;

	void _UpdateButtonPos() noexcept;

	wil::unique_hcursor _hCursor = NULL;

	HWND _hwndBtn1 = NULL;
	HWND _hwndBtn2 = NULL;
	HWND _hwndBtn3 = NULL;
};
