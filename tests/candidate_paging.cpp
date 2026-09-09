#include <cstdlib>
#include <fcitx/candidatelist.h>
#include <fcitx/text.h>
#include <memory>
#include <string>

using namespace fcitx;

static void check(bool condition) {
  if (!condition)
    std::exit(1);
}

static std::unique_ptr<CommonCandidateList> candidateList() {
  auto list = std::make_unique<CommonCandidateList>();
  list->setPageSize(5);
  list->setCursorPositionAfterPaging(CursorPositionAfterPaging::ResetToFirst);
  for (int i = 0; i < 20; ++i)
    list->append(
        std::make_unique<DisplayOnlyCandidateWord>(Text(std::to_string(i))));
  list->setGlobalCursorIndex(0);
  return list;
}

int main() {
  auto list = candidateList();
  list->next();
  check(list->currentPage() == 1);
  check(list->cursorIndex() == 0);
  check(list->globalCursorIndex() == 5);
  check(list->candidate(0).text().toString() == "5");

  list->next();
  check(list->currentPage() == 2);
  check(list->globalCursorIndex() == 10);
  check(list->candidate(0).text().toString() == "10");

  // A global cursor is independent of the visible page. This is the contract
  // that async candidate-list reconstruction must not accidentally rely on.
  auto cursorOnly = candidateList();
  cursorOnly->setGlobalCursorIndex(10);
  check(cursorOnly->currentPage() == 0);
  check(cursorOnly->cursorIndex() == -1);

  auto rebuilt = candidateList();
  rebuilt->setPage(2);
  rebuilt->setGlobalCursorIndex(10);
  check(rebuilt->currentPage() == 2);
  check(rebuilt->cursorIndex() == 0);
  check(rebuilt->candidate(0).text().toString() == "10");
}
