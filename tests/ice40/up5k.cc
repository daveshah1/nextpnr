#include <vector>
#include "gtest/gtest.h"
#include "nextpnr.h"

USING_NEXTPNR_NAMESPACE

class UP5KTest : public ::testing::Test
{
  protected:
    virtual void SetUp()
    {
        IdString::global_ctx = nullptr;
        chipArgs.type = ArchArgs::UP5K;
        chipArgs.package = "sg48";
        ctx = new Context(chipArgs);
    }

    virtual void TearDown() { delete ctx; }

    ArchArgs chipArgs;
    Context *ctx;
};

TEST_F(UP5KTest, bel_names)
{
    int bel_count = 0;
    for (auto bel : ctx->getBels()) {
        auto name = ctx->getBelName(bel);
        ASSERT_EQ(bel, ctx->getBelByName(name));
        bel_count++;
    }
    ASSERT_EQ(bel_count, 5438);
}

TEST_F(UP5KTest, wire_names)
{
    int wire_count = 0;
    for (auto wire : ctx->getWires()) {
        auto name = ctx->getWireName(wire);
        assert(wire == ctx->getWireByName(name));
        wire_count++;
    }
    ASSERT_EQ(wire_count, 103383);
}

TEST_F(UP5KTest, pip_names)
{
    int pip_count = 0;
    for (auto pip : ctx->getPips()) {
        auto name = ctx->getPipName(pip);
        assert(pip == ctx->getPipByName(name));
        pip_count++;
    }
    ASSERT_EQ(pip_count, 1219104);
}

TEST_F(UP5KTest, uphill_to_downhill)
{
    for (auto dst : ctx->getWires()) {
        for (auto uphill_pip : ctx->getPipsUphill(dst)) {
            bool found_downhill = false;
            for (auto downhill_pip : ctx->getPipsDownhill(ctx->getPipSrcWire(uphill_pip))) {
                if (uphill_pip == downhill_pip) {
                    ASSERT_FALSE(found_downhill);
                    found_downhill = true;
                }
            }
            ASSERT_TRUE(found_downhill);
        }
    }
}

TEST_F(UP5KTest, downhill_to_uphill)
{
    for (auto dst : ctx->getWires()) {
        for (auto downhill_pip : ctx->getPipsDownhill(dst)) {
            bool found_uphill = false;
            for (auto uphill_pip : ctx->getPipsUphill(ctx->getPipDstWire(downhill_pip))) {
                if (uphill_pip == downhill_pip) {
                    ASSERT_FALSE(found_uphill);
                    found_uphill = true;
                }
            }
            ASSERT_TRUE(found_uphill);
        }
    }
}
