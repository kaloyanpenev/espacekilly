#include <gtest/gtest.h>

#include <matcher/matcher.h>

TEST(matcherTest, RunApp)
{
	ASSERT_EQ(matcher::runApp(3.14), 0);
}
