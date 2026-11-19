#include "Utils/Utils.h"

#include "Test.h"

class UtilsTest {
protected:
    static Result<int> Half(int value) {
        if (value % 2 != 0) {
            return Err("{} is odd", value);
        }
        return value / 2;
    }

    static Result<> Even(int value) {
        if (value % 2 != 0) {
            return Err("{} is odd", value);
        }
        return {};
    }
};

TEST(UtilsTest, "a result holds a value or says what went wrong") {
    EXPECT_EQ(Half(8).value_or(0), 4);
    EXPECT(!Half(7));
    EXPECT_EQ(Half(7).error().message, "7 is odd");
    EXPECT(Even(2).has_value());
    EXPECT_EQ(Even(3).error().message, "3 is odd");
}
