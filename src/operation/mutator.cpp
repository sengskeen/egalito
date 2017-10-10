#include <algorithm>  // for std::max
#include <iomanip>
#include "mutator.h"
#include "chunk/position.h"
#include "pass/positiondump.h"
#include "instr/instr.h"
#include "log/log.h"

void ChunkMutator::makePositionFor(Chunk *child) {
    PositionFactory *positionFactory = PositionFactory::getInstance();
    Position *pos = nullptr;
    ChunkCursor cursor(chunk, child);
    ChunkCursor prev = cursor;
    if(prev.prev()) {
        pos = positionFactory->makePosition(
            prev.get(), child, prev.get()->getAddress() - chunk->getAddress());
    }
    else {
        Chunk *prevChunk = nullptr;
#if 0
        Chunk *a = chunk, *b = child;
        while(a) {
            ChunkCursor parentCursor(a, b);
            if(parentCursor.getIndex() > 0) {
                prevChunk = parentCursor.get();
                break;
            }

            auto temp = a;
            a = a->getParent();
            b = temp;
        }

        LOG(1, "WE ARE using parent " << (prevChunk ? prevChunk->getName() : ""));
#endif
        pos = positionFactory->makePosition(prevChunk, child, 0);
    }
    child->setPosition(pos);
}

void ChunkMutator::prepend(Chunk *child) {
    if(chunk->getChildren()->genericGetSize() == 0) {
        append(child);
    }
    else {
        insertBefore(chunk->getChildren()->genericGetAt(0), child);
    }
}

void ChunkMutator::append(Chunk *child) {
    // set sibling pointers
    auto prev = chunk->getChildren()->genericGetLast();
    if(prev) {
        setPreviousSibling(child, prev);
        setNextSibling(prev, child);
    }
    else {
        child->setPreviousSibling(nullptr);
        child->setNextSibling(nullptr);
    }

    // set children and parent pointers
    chunk->getChildren()->genericAdd(child);
    child->setParent(chunk);

    if(!child->getPosition()) makePositionFor(child);
    updateSizesAndAuthorities(child);
}

void ChunkMutator::insertAfter(Chunk *insertPoint, Chunk *newChunk) {
    // set sibling pointers
    setPreviousSibling(newChunk, insertPoint);
    if(insertPoint) {
        auto next = insertPoint->getNextSibling();
        if(next) setPreviousSibling(next, newChunk);
        setNextSibling(newChunk, next);
        setNextSibling(insertPoint, newChunk);
    }
    else {
        newChunk->setNextSibling(nullptr);
    }

    // set children and parent pointers
    auto list = chunk->getChildren();
    size_t index = (insertPoint ? list->genericIndexOf(insertPoint) + 1 : 0);
    list->genericInsertAt(index, newChunk);
    newChunk->setParent(chunk);

    if(!newChunk->getPosition()) makePositionFor(newChunk);
    updateSizesAndAuthorities(newChunk);
}

void ChunkMutator::insertBefore(Chunk *insertPoint, Chunk *newChunk) {
    if(!insertPoint) {
        append(newChunk);
        return;
    }

    // set sibling pointers
    auto prev = insertPoint->getPreviousSibling();
    if(prev) setNextSibling(prev, newChunk);
    setNextSibling(newChunk, insertPoint);
    setPreviousSibling(newChunk, prev);
    setPreviousSibling(insertPoint, newChunk);

    // set children and parent pointers
    auto list = chunk->getChildren();
    size_t index = list->genericIndexOf(insertPoint);
    list->genericInsertAt(index, newChunk);
    newChunk->setParent(chunk);

    // must run before the first-entry update below
    if(!newChunk->getPosition()) makePositionFor(newChunk);

    if(PositionFactory::getInstance()->needsSpecialCaseFirst()
        && !newChunk->getPreviousSibling()) {

        // We are replacing the first entry in a block, which is
        // special-cased to an OffsetPosition. We should make sure
        // the first and only the first entry is an OffsetPosition.
        // Maintain that invariant.

        PositionFactory *positionFactory = PositionFactory::getInstance();
        delete insertPoint->getPosition();
        insertPoint->setPosition(positionFactory->makePosition(
            newChunk, insertPoint, newChunk->getSize()));
    }

    updateSizesAndAuthorities(newChunk);
}

void ChunkMutator::insertBeforeJumpTo(Instruction *insertPoint, Instruction *newChunk) {
    if(insertPoint == nullptr) {
        insertBefore(nullptr, newChunk);
        return;
    }

    insertAfter(insertPoint, newChunk);

    // swap semantics of these two instructions
    auto sem1 = insertPoint->getSemantic();
    auto sem2 = newChunk->getSemantic();
    insertPoint->setSemantic(sem2);
    newChunk->setSemantic(sem1);
}

void ChunkMutator::remove(Chunk *child) {
    // set sibling pointers
    auto prev = child->getPreviousSibling();
    auto next = child->getNextSibling();
    if(prev) {
        setNextSibling(prev, next);
    }
    if(next) {
        setPreviousSibling(next, prev);
    }

    // remove from parent
    chunk->getChildren()->genericRemove(child);

    // update sizes of parents and grandparents
    for(Chunk *c = chunk; c; c = c->getParent()) {
        // only if size is tracked
        if(c->getSize() != 0) {
            c->addToSize(-child->getSize());
        }
    }

    // update authority pointers in positions
    updateGenerationCounts(child);
}

void ChunkMutator::splitBlockBefore(Instruction *point) {
#if 0
    auto block = dynamic_cast<Block *>(point->getParent());
    Block *block2 = nullptr;

    PositionFactory *positionFactory = PositionFactory::getInstance();

    std::vector<Instruction *> moveList;
    for(auto child : CIter::children(block)) {
        if(!block2) {
            if(child == point) {
                block2 = new Block();
            }
        }
        if(block2) {
            moveList.push_back(child);
        }
    }

    for(auto child : moveList) {
        ChunkMutator(block).remove(child);
        delete child->getPosition();
    }
    insertAfter(block, block2);
    Chunk *prevChunk = block;
    for(auto child : moveList) {
        child->setPosition(positionFactory->makePosition(
            prevChunk, child, block2->getSize()));

        ChunkMutator(block2).append(child);
        prevChunk = child;
    }
    if(auto block3 = dynamic_cast<Block *>(block2->getNextSibling())) {
        auto instr = block3->getChildren()->getIterable()->get(0);
        delete instr->getPosition();
        instr->setPosition(positionFactory->makePosition(
            block2, instr, block2->getSize()));
    }
#else
    Block *block = dynamic_cast<Block *>(point->getParent());
    Block *newBlock = new Block();

    newBlock->setPosition(PositionFactory::getInstance()->makePosition(
        block, newBlock, point->getAddress() - chunk->getAddress()));

    size_t totalChildren = block->getChildren()->getIterable()->getCount();
    if(totalChildren == 0) return;

    size_t leaveBehind = 0;
    for(Instruction *instr = block->getChildren()->getIterable()->get(0);
        instr && instr != point;
        instr = static_cast<Instruction *>(instr->getNextSibling())) {

        leaveBehind ++;
    }
    if(auto lastLeftBehind = point->getPreviousSibling()) {
        setNextSibling(lastLeftBehind, nullptr);
    }
    setPreviousSibling(point, nullptr);

    {
        ChunkMutator newMutator(newBlock);
        Chunk *prevChunk = newBlock;
        for(size_t i = leaveBehind; i < totalChildren; i ++) {
            Instruction *instr = block->getChildren()->getIterable()->get(i);

            delete instr->getPosition();
            instr->setPosition(PositionFactory::getInstance()->makePosition(
                prevChunk, instr, newBlock->getSize()));
            newMutator.append(instr);
            prevChunk = instr;
        }
    }

    for(size_t i = 0; i < totalChildren - leaveBehind; i ++) {
        block->getChildren()->removeLast();
    }

    insertAfter(block, newBlock);
    if(auto block3 = dynamic_cast<Block *>(block->getNextSibling())) {
        setPreviousSibling(block3, newBlock);
        setNextSibling(newBlock, block3);
    }
    setNextSibling(block, newBlock);
    setPreviousSibling(newBlock, block);
#endif
}

void ChunkMutator::modifiedChildSize(Chunk *child, int added) {
    // update sizes of parents and grandparents
    for(Chunk *c = chunk; c; c = c->getParent()) {
        c->addToSize(added);
    }

    // update authority pointers in positions
    updateGenerationCounts(child);
}

void ChunkMutator::setPosition(address_t address) {
    chunk->getPosition()->set(address);
}

void ChunkMutator::setPreviousSibling(Chunk *c, Chunk *prev) {
    c->setPreviousSibling(prev);
    if(!prev) return;
    if(auto v = dynamic_cast<SubsequentPosition *>(c->getPosition())) {
        v->setAfterThis(prev);
    }
}

void ChunkMutator::setNextSibling(Chunk *c, Chunk *next) {
    c->setNextSibling(next);
    if(!next) return;
    if(auto v = dynamic_cast<SubsequentPosition *>(next->getPosition())) {
        v->setAfterThis(c);
    }
}

void ChunkMutator::updateSizesAndAuthorities(Chunk *child) {
    // update sizes of parents and grandparents
    for(Chunk *c = chunk; c; c = c->getParent()) {
        c->addToSize(child->getSize());
    }

    // update authority pointers in positions
    updateGenerationCounts(child);
}

void ChunkMutator::updateGenerationCounts(Chunk *child) {
    if(!PositionFactory::getInstance()->needsGenerationTracking()) return;

    // first, find the max of all generations from child on up
    int gen = 0;
    for(Chunk *c = child; c; c = c->getParent()) {
        gen = std::max(gen, c->getPosition()->getGeneration());
        if(dynamic_cast<AbsolutePosition *>(c->getPosition())) break;
    }
    gen ++;  // increment generation by one

    // now, set generations of child and up to higher numbers
    for(Chunk *c = child; c; c = c->getParent()) {
        c->getPosition()->setGeneration(gen);
        if(dynamic_cast<AbsolutePosition *>(c->getPosition())) break;

        // NOTE: each parent has a higher generation number. This ensures that
        // the authority has a higher number than any of its dependencies,
        // and lookups in any siblings will see this higher number.
        gen ++;
    }

    updateAuthorityHelper(child);
}

void ChunkMutator::updatePositions() {
    if(!allowUpdates) return;
    if(!PositionFactory::getInstance()->needsUpdatePasses()) return;

    for(Chunk *c = chunk; c; c = c->getParent()) {
        if(dynamic_cast<AbsolutePosition *>(c->getPosition())) {
            updatePositionHelper(c);
            //PositionDump().visit(c);
        }
    }
}

void ChunkMutator::updateAuthorityHelper(Chunk *root) {
    root->getPosition()->updateAuthority();

    if(root->getChildren()) {
        for(auto child : root->getChildren()->genericIterable()) {
            updateAuthorityHelper(child);
        }
    }
}

void ChunkMutator::updatePositionHelper(Chunk *root) {
    // Must recalculate root's position before descending into children,
    // since some Position types depend on parents.
    root->getPosition()->recalculate();

    if(root->getChildren()) {
        for(auto child : root->getChildren()->genericIterable()) {
            updatePositionHelper(child);
        }
    }
}
