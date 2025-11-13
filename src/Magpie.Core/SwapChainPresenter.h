#pragma once
#include "PresenterBase.h"

namespace Magpie {

class SwapChainPresenter {
public:
	SwapChainPresenter() = default;
	SwapChainPresenter(const SwapChainPresenter&) = delete;
	SwapChainPresenter(SwapChainPresenter&&) = delete;

	~SwapChainPresenter();
};

}
