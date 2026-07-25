// Copyright Solessfir. All Rights Reserved.

#pragma once

#include "Modules/ModuleManager.h"

class FEnhancedAbilitySystemModule : public IModuleInterface
{
public:
	virtual void StartupModule() override;

	virtual void ShutdownModule() override;
};
