/*
**  Copyright (C) 2012 Aldebaran Robotics
**  See COPYING for the license
*/

#include <qicore/file.hpp>
#include <qi/anymodule.hpp>

namespace qi
{
  class ProgressNotifierImpl
    : public ProgressNotifier
  {
  public:
    explicit ProgressNotifierImpl(Future<void> operationFuture)
      : _opFuture(std::move(operationFuture))
    {
      this->status.set(ProgressNotifier::Status_Idle);
    }

    void reset() override
    {
      this->status.set(ProgressNotifier::Status_Idle);
      this->progress.set(0.0);
    }

    void notifyRunning() override
    {
      if (this->status.get() != ProgressNotifier::Status_Idle)
        qiLogError("qicore.file.progressnotifierbase")
        << "ProgressNotifier must be Idle to be allowed to switch to Running status.";

      this->status.set(ProgressNotifier::Status_Running);
    }

    void notifyFinished() override
    {
      if (!isRunning())
        qiLogError("qicore.file.progressnotifierbase")
        << "ProgressNotifier must be Running to be allowed to switch to Finished status.";

      this->status.set(ProgressNotifier::Status_Finished);
    }

    void notifyCanceled() override
    {
      if (!isRunning())
        qiLogError("qicore.file.progressnotifierbase")
        << "ProgressNotifier must be Running to be allowed to switch to Canceled status.";
      this->status.set(ProgressNotifier::Status_Canceled);
    }

    void notifyFailed() override
    {
      if (!isRunning())
        qiLogError("qicore.file.progressnotifierbase")
        << "ProgressNotifier must be Running to be allowed to switch to Failed status.";
      this->status.set(ProgressNotifier::Status_Failed);
    }

    void notifyProgressed(double newProgress) override
    {
      if (!isRunning())
        qiLogError("qicore.file.progressnotifierbase")
        << "ProgressNotifier must be Running to be allowed to notify any progress.";
      this->progress.set(newProgress);
    }

    bool isRunning() const override
    {
      return this->status.get() == ProgressNotifier::Status_Running;
    }

    Future<void> waitForFinished() override
    {
      return _opFuture;
    }

    Future<void> _opFuture;
  };


void _qiregisterProgressNotifier()
{
  ::qi::ObjectTypeBuilder<ProgressNotifier> builder;
  QI_OBJECT_BUILDER_ADVERTISE(builder, ProgressNotifier, notifyRunning);
  QI_OBJECT_BUILDER_ADVERTISE(builder, ProgressNotifier, notifyFinished);
  QI_OBJECT_BUILDER_ADVERTISE(builder, ProgressNotifier, notifyCanceled);
  QI_OBJECT_BUILDER_ADVERTISE(builder, ProgressNotifier, notifyFailed);
  QI_OBJECT_BUILDER_ADVERTISE(builder, ProgressNotifier, notifyProgressed);
  QI_OBJECT_BUILDER_ADVERTISE(builder, ProgressNotifier, waitForFinished);
  QI_OBJECT_BUILDER_ADVERTISE(builder, ProgressNotifier, isRunning);
  QI_OBJECT_BUILDER_ADVERTISE(builder, ProgressNotifier, reset);
  QI_OBJECT_BUILDER_ADVERTISE(builder, ProgressNotifier, progress);
  QI_OBJECT_BUILDER_ADVERTISE(builder, ProgressNotifier, status);

  builder.registerType();

  {
    qi::detail::ForceProxyInclusion<ProgressNotifier>().dummyCall();
    qi::registerType(typeid(ProgressNotifierImpl), qi::typeOf<ProgressNotifier>());
    ProgressNotifierImpl* ptr = static_cast<ProgressNotifierImpl*>(reinterpret_cast<void*>(0x10000));
    ProgressNotifier* pptr = ptr;
    intptr_t offset = reinterpret_cast<intptr_t>(pptr)-reinterpret_cast<intptr_t>(ptr);
    if (offset)
    {
      qiLogError("qitype.register") << "non-zero offset for implementation ProgressNotifierImpl of ProgressNotifier,"
        "call will fail at runtime";
      throw std::runtime_error("non-zero offset between implementation and interface");
    }
  }

}

ProgressNotifierPtr createProgressNotifier(Future<void> operationFuture)
{
  return boost::make_shared<ProgressNotifierImpl>(std::move(operationFuture));
}

void registerProgressNotifierCreation(qi::ModuleBuilder& mb)
{
  mb.advertiseMethod("createProgressNotifier", &createProgressNotifier);
}
}
