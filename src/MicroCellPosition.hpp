#pragma once

#include <stdexcept>

#include "Direction.hpp"

struct MicroCellPosition
{
	int x;
	int y;

	inline MicroCellPosition& operator+=(Direction direction)
	{
		switch (direction) {
			case Right: this->x += 1; break;
			case Up: this->y -= 1; break;
			case Left: this->x -= 1; break;
			case Down: this->y += 1; break;
			default: throw std::out_of_range("direction");
		}
		return *this;
	}
};

// Adapted from https://stackoverflow.com/a/14010215
struct BoundedSpiral
{
private:
	enum struct BoundCheck
	{
		Negative = -1,
		Inside   = 0,
		Positive = 1,
	};

public:
	MicroCellPosition pos;
	BoundedSpiral(MicroCellPosition pos, int32_t max_x, int32_t max_y)
	    : pos(pos), max_x(max_x), max_y(max_y), layer(1), leg(0), cur_x(0), cur_y(0)
	{
		x_valid = bounds_check(pos.x, max_x);
		y_valid = bounds_check(pos.y, max_y);
	}

	bool next()
	{
		switch (leg)
		{
			case 0: // RIGHT
				++pos.x; if (x_valid <= BoundCheck::Inside) x_valid = bounds_check(pos.x, max_x);
				++cur_x; if (cur_x == layer) { ++leg; }
				break;
			case 1: // UP
				--pos.y; if (y_valid >= BoundCheck::Inside) y_valid = bounds_check(pos.y, max_y);
				--cur_y; if (-cur_y == layer) { ++leg; }
				break;
			case 2: // LEFT
				--pos.x; if (x_valid >= BoundCheck::Inside) x_valid = bounds_check(pos.x, max_x);
				--cur_x; if (-cur_x == layer) { ++leg; }
				break;
			case 3: // DOWN
				++pos.y; if (y_valid <= BoundCheck::Inside) y_valid = bounds_check(pos.y, max_y);
				++cur_y; if (cur_y == layer) { leg = 0; ++layer; }
				break;
		}
		return (x_valid == BoundCheck::Inside) && (y_valid == BoundCheck::Inside);
	}

	inline void next_valid()
	{
		while (next() == false) {}
	}

	inline int32_t to_array_index() const
	{
		return (pos.x * max_y) + pos.y;
	}

	inline BoundCheck bounds_check(int32_t const& check, int32_t const& max) const
	{
		if (check < 0) return BoundCheck::Negative;
		else if (check >= max) return BoundCheck::Positive;
		else return BoundCheck::Inside;
	}

private:
	uint32_t layer, leg;
	BoundCheck x_valid, y_valid;
	int32_t cur_x, max_x;
	int32_t cur_y, max_y;
};
