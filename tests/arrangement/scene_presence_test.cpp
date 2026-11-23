#include <catch2/catch_test_macros.hpp>
#include <memory>
#include <optional>

#include "arrangement/edit_compiler.h"
#include "arrangement/edit_history.h"

namespace arr = sonare::arrangement;
namespace mixing = sonare::mixing::api;

TEST_CASE("Reading or compiling an unset mixer scene leaves it unset",
          "[arrangement][scene_presence]") {
  arr::Project project;
  CHECK(project.find_scene() == nullptr);
  const arr::Project& read_only = project;
  CHECK(read_only.scene().version == mixing::kSceneFormatVersion);
  CHECK(project.find_scene() == nullptr);

  const auto absent = arr::compile(project, {}, {});
  REQUIRE(absent.timeline);
  CHECK(project.find_scene() == nullptr);

  project.scene();
  REQUIRE(project.find_scene() != nullptr);
  const auto present = arr::compile(project, {}, {});
  REQUIRE(present.timeline);
  CHECK(mixing::scene_to_json(absent.timeline->mixer.scene) ==
        mixing::scene_to_json(present.timeline->mixer.scene));
}

TEST_CASE("Scene edits preserve absence and explicit defaults through undo and redo",
          "[arrangement][scene_presence]") {
  arr::EditHistory history{arr::Project{}};
  CHECK(history.project().find_scene() == nullptr);

  REQUIRE(history.apply(std::make_unique<arr::SetScene>(mixing::scene_from_json("{}"))));
  REQUIRE(history.project().find_scene() != nullptr);
  CHECK(history.project().find_scene()->version == mixing::kSceneFormatVersion);
  REQUIRE(history.undo());
  CHECK(history.project().find_scene() == nullptr);
  REQUIRE(history.redo());
  REQUIRE(history.project().find_scene() != nullptr);

  REQUIRE(history.apply(std::make_unique<arr::SetScene>(std::nullopt)));
  CHECK(history.project().find_scene() == nullptr);
  REQUIRE(history.undo());
  REQUIRE(history.project().find_scene() != nullptr);
  REQUIRE(history.redo());
  CHECK(history.project().find_scene() == nullptr);
}
