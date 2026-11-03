#include "services/ExtensionsService.h"

#include "services/DeviceService.h"
#include "services/HangTrace.h"

void ExtensionsService::begin()
{
    for (ExtensionModule *extension : modules)
    {
        extension->begin();
    }
}

void ExtensionsService::configure()
{
    for (ExtensionModule *extension : modules)
    {
        extension->configure();
    }
}

std::span<ExtensionModule *const> ExtensionsService::getAll() { return modules; }

void ExtensionsService::handle()
{
    for (size_t index{0U}; index < modules.size(); ++index)
    {
        HangTrace::markExtension(index);
        modules[index]->handle();
    }
}
