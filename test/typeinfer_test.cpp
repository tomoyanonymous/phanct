#include "compiler/alphaconvert_visitor.hpp"
#include "gtest/gtest.h"
#include "gtest/internal/gtest-port.h"
#include "basic/helper_functions.hpp"
#include "runtime/runtime.hpp"
#include "compiler/type_infer_visitor.hpp"

static mimium::Runtime runtime;
static std::shared_ptr<mimium::AlphaConvertVisitor> alphaconv;
static std::shared_ptr<mimium::TypeInferVisitor> typeinfer;

TEST(typeinfertest, basic) {
  alphaconv = std::make_shared<mimium::AlphaConvertVisitor>();
  typeinfer = std::make_shared<mimium::TypeInferVisitor>();
  runtime.setWorkingDirectory("/Users/tomoya/codes/mimium/build/test/");
  mimium::Logger::current_report_level = mimium::Logger::INFO;
  runtime.init(alphaconv);
  runtime.loadSourceFile("test_typeident.mmm");
  auto mainast = alphaconv->getResult();
  mimium::Logger::debug_log(mainast->toString(), mimium::Logger::DEBUG);
  mainast->accept(*typeinfer);
  auto env = typeinfer->getEnv().env;
  for (auto& it : env) {
    std::cout << it.first << " : "
              << std::visit([](auto t) { return t.toString(); }, it.second)
              << std::endl;
  }
  auto typestr = std::visit([](auto t) { return t.toString(); }, env["hof"]);
auto typestr2 = std::visit([](auto t) { return t.toString(); }, env["hof2"]);

  EXPECT_EQ(typestr, "Fn[ (Float , Fn[ (Float) -> Float ]) -> Float ]");
  EXPECT_EQ(typestr2, "Fn[ (Float , Fn[ (Float , Float) -> Float ]) -> Fn[ (Float) -> Float ] ]");

}