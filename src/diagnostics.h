#pragma once

#include <QStringList>

// فحوص بلا واجهة تُستخدم للتحقق من الدقة والأداء (تشغّلها GitHub Actions أيضاً)
namespace Diagnostics {

// يعيد -1 إذا لم تكن الوسائط أمراً تشخيصياً، وإلا يعيد رمز الخروج
int run(const QStringList &args);

} // namespace Diagnostics
