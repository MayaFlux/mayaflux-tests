#include "gtest/gtest.h"

#include "MayaFlux/Kinesis/MotionCurves.hpp"

namespace MayaFlux::Test {

namespace {

    using Kinesis::CurveEvaluator;
    using Kinesis::InterpolationMode;

    constexpr double TOL = 1e-9;

    using Point = std::array<double, 3>;

    struct Curve {
        std::vector<double> out;
        std::vector<double> tangents;
        size_t samples {};

        [[nodiscard]] double at(size_t d, size_t i) const { return out[d * samples + i]; }
        [[nodiscard]] double slope(size_t d, size_t i) const { return tangents[d * samples + i]; }
    };

    std::vector<double> interleave(const std::vector<Point>& pts)
    {
        std::vector<double> flat;
        flat.reserve(pts.size() * 3);
        for (const auto& p : pts) {
            flat.insert(flat.end(), p.begin(), p.end());
        }
        return flat;
    }

    Eigen::MatrixXd as_columns(const std::vector<Point>& pts)
    {
        Eigen::MatrixXd m(3, static_cast<Eigen::Index>(pts.size()));
        for (size_t i = 0; i < pts.size(); ++i) {
            for (size_t d = 0; d < 3; ++d) {
                m(static_cast<Eigen::Index>(d), static_cast<Eigen::Index>(i)) = pts[i][d];
            }
        }
        return m;
    }

    Curve evaluate(InterpolationMode mode, double tension, const std::vector<Point>& pts, Eigen::Index samples)
    {
        CurveEvaluator evaluator(mode, tension);
        Curve c;
        c.samples = static_cast<size_t>(samples);
        evaluator.evaluate_planar(interleave(pts), 3, samples, c.out, c.tangents);
        return c;
    }

    Point combine(const std::vector<std::pair<double, Point>>& weighted)
    {
        Point r { 0.0, 0.0, 0.0 };
        for (const auto& [w, p] : weighted) {
            for (size_t d = 0; d < 3; ++d) {
                r[d] += w * p[d];
            }
        }
        return r;
    }

    void expect_sample(const Curve& c, size_t i, const Point& expected, double tol = TOL)
    {
        for (size_t d = 0; d < 3; ++d) {
            EXPECT_NEAR(c.at(d, i), expected[d], tol) << "sample " << i << " component " << d;
        }
    }

    void expect_vector(const Eigen::VectorXd& v, const Point& expected, double tol = TOL)
    {
        ASSERT_EQ(v.size(), 3);
        for (size_t d = 0; d < 3; ++d) {
            EXPECT_NEAR(v(static_cast<Eigen::Index>(d)), expected[d], tol) << "component " << d;
        }
    }

    std::vector<Point> four_points()
    {
        return { { 0, 0, 0 }, { 1, 2, 3 }, { 4, 1, 2 }, { 5, 5, 5 } };
    }

    std::vector<Point> line_points(size_t count)
    {
        std::vector<Point> pts;
        pts.reserve(count);
        for (size_t i = 0; i < count; ++i) {
            const auto f = static_cast<double>(i);
            pts.push_back({ 1.0 + f, 1.0 + 2.0 * f, 1.0 + 3.0 * f });
        }
        return pts;
    }

    std::vector<Point> positive_random_points(size_t count)
    {
        std::vector<Point> pts;
        pts.reserve(count);
        uint64_t state = 12345;
        const auto next = [&state]() {
            state = (state * 1103515245ULL + 12345ULL) % 2147483648ULL;
            return 1.0 + static_cast<double>(state) / 2147483648.0;
        };
        for (size_t i = 0; i < count; ++i) {
            pts.push_back({ next(), next(), next() });
        }
        return pts;
    }

    class CurveEvaluatorTest : public ::testing::Test {
    };

    TEST_F(CurveEvaluatorTest, CatmullRomOfFourControlsRunsFromTheSecondToTheThird)
    {
        const auto pts = four_points();
        const auto c = evaluate(InterpolationMode::CATMULL_ROM, 0.5, pts, 3);

        expect_sample(c, 0, pts[1]);
        expect_sample(c, 2, pts[2]);
    }

    TEST_F(CurveEvaluatorTest, CatmullRomMidpointIsTheClassicWeighting)
    {
        const auto pts = four_points();
        const auto c = evaluate(InterpolationMode::CATMULL_ROM, 0.5, pts, 3);

        expect_sample(c, 1,
            combine({ { -1.0 / 16.0, pts[0] }, { 9.0 / 16.0, pts[1] }, { 9.0 / 16.0, pts[2] }, { -1.0 / 16.0, pts[3] } }));
    }

    TEST_F(CurveEvaluatorTest, RadialFanCubicSweepsFromNearOriginToFourthControl)
    {
        const auto c = evaluate(InterpolationMode::RADIAL_FAN_CUBIC, 0.5, four_points(), 3);

        expect_sample(c, 0, { -0.5, -1.0, -1.5 });
        expect_sample(c, 1, { 2.0, 1.4375, 1.625 });
        expect_sample(c, 2, { 5.0, 5.0, 5.0 });
    }

    TEST_F(CurveEvaluatorTest, RadialFanCubicSingleSampleMatchesTheSampledCurve)
    {
        const auto points = as_columns(four_points());

        expect_vector(Kinesis::interpolate(points, 0.0,
                          InterpolationMode::RADIAL_FAN_CUBIC),
            { -0.5, -1.0, -1.5 });
        expect_vector(Kinesis::interpolate(points, 0.5,
                          InterpolationMode::RADIAL_FAN_CUBIC),
            { 2.0, 1.4375, 1.625 });
        expect_vector(Kinesis::interpolate(points, 1.0,
                          InterpolationMode::RADIAL_FAN_CUBIC),
            { 5.0, 5.0, 5.0 });
    }

    TEST_F(CurveEvaluatorTest, CatmullRomTangentsAreHalfTheSpanOfTheNeighbours)
    {
        const auto pts = four_points();
        const auto c = evaluate(InterpolationMode::CATMULL_ROM, 0.5, pts, 3);

        for (size_t d = 0; d < 3; ++d) {
            EXPECT_NEAR(c.slope(d, 0), 0.5 * (pts[2][d] - pts[0][d]), TOL) << "start, component " << d;
            EXPECT_NEAR(c.slope(d, 2), 0.5 * (pts[3][d] - pts[1][d]), TOL) << "end, component " << d;
        }
    }

    TEST_F(CurveEvaluatorTest, TensionZeroMakesTheMidpointTheAverageOfTheInnerControls)
    {
        const auto pts = four_points();
        const auto c = evaluate(InterpolationMode::CATMULL_ROM, 0.0, pts, 3);

        expect_sample(c, 1, combine({ { 0.5, pts[1] }, { 0.5, pts[2] } }));
    }

    TEST_F(CurveEvaluatorTest, TensionChangesTheShapeButNotTheEndpoints)
    {
        const auto pts = four_points();
        const auto soft = evaluate(InterpolationMode::CATMULL_ROM, 0.5, pts, 3);
        const auto stiff = evaluate(InterpolationMode::CATMULL_ROM, 1.0, pts, 3);

        expect_sample(stiff, 0, pts[1]);
        expect_sample(stiff, 2, pts[2]);
        double difference = 0.0;
        for (size_t d = 0; d < 3; ++d) {
            difference = std::max(difference, std::abs(soft.at(d, 1) - stiff.at(d, 1)));
        }
        EXPECT_GT(difference, 1e-6);
    }

    TEST_F(CurveEvaluatorTest, BSplineFollowsTheUniformCubicWeights)
    {
        const auto pts = four_points();
        const auto c = evaluate(InterpolationMode::BSPLINE, 0.5, pts, 3);

        expect_sample(c, 0,
            combine({ { 1.0 / 6.0, pts[0] }, { 4.0 / 6.0, pts[1] }, { 1.0 / 6.0, pts[2] } }));
        expect_sample(c, 1,
            combine({ { 1.0 / 48.0, pts[0] }, { 23.0 / 48.0, pts[1] }, { 23.0 / 48.0, pts[2] }, { 1.0 / 48.0, pts[3] } }));
        expect_sample(c, 2,
            combine({ { 1.0 / 6.0, pts[1] }, { 4.0 / 6.0, pts[2] }, { 1.0 / 6.0, pts[3] } }));
    }

    TEST_F(CurveEvaluatorTest, CubicBezierInterpolatesItsEndsAndBlendsTheMiddle)
    {
        const auto pts = four_points();
        const auto c = evaluate(InterpolationMode::CUBIC_BEZIER, 0.5, pts, 3);

        expect_sample(c, 0, pts[0]);
        expect_sample(c, 1,
            combine({ { 1.0 / 8.0, pts[0] }, { 3.0 / 8.0, pts[1] }, { 3.0 / 8.0, pts[2] }, { 1.0 / 8.0, pts[3] } }));
        expect_sample(c, 2, pts[3]);
    }

    TEST_F(CurveEvaluatorTest, QuadraticBezierInterpolatesItsEndsAndBlendsTheMiddle)
    {
        const std::vector<Point> pts { { 0, 0, 0 }, { 2, 4, 6 }, { 4, 0, 2 } };
        const auto c = evaluate(InterpolationMode::QUADRATIC_BEZIER, 0.5, pts, 3);

        expect_sample(c, 0, pts[0]);
        expect_sample(c, 1, combine({ { 0.25, pts[0] }, { 0.5, pts[1] }, { 0.25, pts[2] } }));
        expect_sample(c, 2, pts[2]);
    }

    TEST_F(CurveEvaluatorTest, CubicHermiteTakesEndpointsThenEndTangents)
    {
        const std::vector<Point> pts { { 0, 0, 0 }, { 1, 1, 1 }, { 2, 0, 0 }, { 0, 2, 0 } };
        const auto c = evaluate(InterpolationMode::CUBIC_HERMITE, 0.5, pts, 3);

        expect_sample(c, 0, pts[0]);
        expect_sample(c, 1, { 0.75, 0.25, 0.5 });
        expect_sample(c, 2, pts[1]);
        for (size_t d = 0; d < 3; ++d) {
            EXPECT_NEAR(c.slope(d, 0), pts[2][d], TOL) << "start tangent, component " << d;
            EXPECT_NEAR(c.slope(d, 2), pts[3][d], TOL) << "end tangent, component " << d;
        }
    }

    TEST_F(CurveEvaluatorTest, LinearPassesThroughEveryControlOfAMultiSegmentPath)
    {
        const std::vector<Point> pts { { 0, 0, 0 }, { 2, 4, 6 }, { 6, 4, 2 } };
        const auto c = evaluate(InterpolationMode::LINEAR, 0.5, pts, 5);

        expect_sample(c, 0, pts[0]);
        expect_sample(c, 1, combine({ { 0.5, pts[0] }, { 0.5, pts[1] } }));
        expect_sample(c, 2, pts[1]);
        expect_sample(c, 3, combine({ { 0.5, pts[1] }, { 0.5, pts[2] } }));
        expect_sample(c, 4, pts[2]);
    }

    TEST_F(CurveEvaluatorTest, CosineBlendsTwoControlsThroughTheirAverage)
    {
        const std::vector<Point> pts { { 0, 0, 0 }, { 2, 4, 6 } };
        const auto c = evaluate(InterpolationMode::COSINE, 0.5, pts, 3);

        expect_sample(c, 0, pts[0]);
        expect_sample(c, 1, { 1, 2, 3 });
        expect_sample(c, 2, pts[1]);
    }

    TEST_F(CurveEvaluatorTest, ConstructionReportsItsModeAndTension)
    {
        CurveEvaluator evaluator(InterpolationMode::BSPLINE, 0.25);

        EXPECT_EQ(evaluator.mode(), InterpolationMode::BSPLINE);
        EXPECT_DOUBLE_EQ(evaluator.tension(), 0.25);
    }

    TEST_F(CurveEvaluatorTest, ConfigureSwitchesTheKernel)
    {
        CurveEvaluator evaluator(InterpolationMode::CATMULL_ROM, 0.5);
        std::vector<double> out;
        std::vector<double> tangents;
        const std::vector<Point> two { { 0, 0, 0 }, { 2, 4, 6 } };

        evaluator.evaluate_planar(interleave(four_points()), 3, 3, out, tangents);

        evaluator.configure(InterpolationMode::LINEAR, 0.5);
        evaluator.evaluate_planar(interleave(two), 3, 3, out, tangents);

        EXPECT_EQ(evaluator.mode(), InterpolationMode::LINEAR);
        EXPECT_NEAR(out[1], 1.0, TOL);
        EXPECT_NEAR(out[3 + 1], 2.0, TOL);
        EXPECT_NEAR(out[6 + 1], 3.0, TOL);
    }

    TEST_F(CurveEvaluatorTest, ConfigureWithANewTensionRebuildsTheCurve)
    {
        CurveEvaluator evaluator(InterpolationMode::CATMULL_ROM, 0.5);
        std::vector<double> out;
        std::vector<double> tangents;
        const auto pts = four_points();

        evaluator.configure(InterpolationMode::CATMULL_ROM, 0.0);
        evaluator.evaluate_planar(interleave(pts), 3, 3, out, tangents);

        EXPECT_NEAR(out[1], 0.5 * (pts[1][0] + pts[2][0]), TOL);
    }

    TEST_F(CurveEvaluatorTest, SamplesBelowTwoAreRejected)
    {
        CurveEvaluator evaluator(InterpolationMode::LINEAR, 0.5);
        std::vector<double> out;
        std::vector<double> tangents;

        EXPECT_THROW(evaluator.evaluate_planar(interleave({ { 0, 0, 0 }, { 1, 1, 1 } }), 3, 1, out, tangents), std::invalid_argument);
    }

    TEST_F(CurveEvaluatorTest, ZeroDimensionIsRejected)
    {
        CurveEvaluator evaluator(InterpolationMode::LINEAR, 0.5);
        std::vector<double> out;
        std::vector<double> tangents;

        EXPECT_THROW(evaluator.evaluate_planar(std::vector<double> { 0, 1 }, 0, 4, out, tangents), std::invalid_argument);
    }

    TEST_F(CurveEvaluatorTest, SingleControlIsRejected)
    {
        CurveEvaluator evaluator(InterpolationMode::LINEAR, 0.5);
        std::vector<double> out;
        std::vector<double> tangents;

        EXPECT_THROW(evaluator.evaluate_planar(interleave({ { 0, 0, 0 } }), 3, 4, out, tangents), std::invalid_argument);
    }

    TEST_F(CurveEvaluatorTest, CatmullRomNeedsAtLeastFourControls)
    {
        CurveEvaluator evaluator(InterpolationMode::CATMULL_ROM, 0.5);
        std::vector<double> out;
        std::vector<double> tangents;

        EXPECT_THROW(evaluator.evaluate_planar(interleave(line_points(3)), 3, 8, out, tangents), std::invalid_argument);
    }

    TEST_F(CurveEvaluatorTest, HermiteTakesExactlyFourControls)
    {
        CurveEvaluator evaluator(InterpolationMode::CUBIC_HERMITE, 0.5);
        std::vector<double> out;
        std::vector<double> tangents;

        EXPECT_THROW(evaluator.evaluate_planar(interleave(line_points(5)), 3, 8, out, tangents), std::invalid_argument);
        EXPECT_NO_THROW(evaluator.evaluate_planar(interleave(line_points(4)), 3, 8, out, tangents));
    }

    TEST_F(CurveEvaluatorTest, CustomModeIsNotEvaluatedHere)
    {
        CurveEvaluator evaluator(InterpolationMode::CUSTOM, 0.5);
        std::vector<double> out;
        std::vector<double> tangents;

        EXPECT_THROW(evaluator.evaluate_planar(interleave(line_points(4)), 3, 8, out, tangents), std::invalid_argument);
    }

    TEST_F(CurveEvaluatorTest, MatrixOverloadAgreesWithThePlanarEntryPoint)
    {
        const auto pts = positive_random_points(6);
        const auto planar = evaluate(InterpolationMode::CATMULL_ROM, 0.5, pts, 41);

        CurveEvaluator evaluator(InterpolationMode::CATMULL_ROM, 0.5);
        Eigen::MatrixXd out;
        evaluator.evaluate(as_columns(pts), 41, out);

        ASSERT_EQ(out.rows(), 3);
        ASSERT_EQ(out.cols(), 41);
        for (size_t i = 0; i < 41; ++i) {
            for (size_t d = 0; d < 3; ++d) {
                EXPECT_NEAR(out(static_cast<Eigen::Index>(d), static_cast<Eigen::Index>(i)), planar.at(d, i), TOL)
                    << "sample " << i << " component " << d;
            }
        }
    }

    TEST_F(CurveEvaluatorTest, PlanarMatchesTheSingleSampleFunctionsAlongAWholeSegment)
    {
        const auto pts = four_points();
        const auto matrix = as_columns(pts);
        constexpr size_t samples = 9;

        struct Case {
            InterpolationMode mode;
            const char* name;
        };
        for (const auto& [mode, name] : { Case { InterpolationMode::CATMULL_ROM, "catmull_rom" },
                 Case { InterpolationMode::BSPLINE, "bspline" },
                 Case { InterpolationMode::CUBIC_BEZIER, "cubic_bezier" } }) {
            SCOPED_TRACE(name);

            const auto c = evaluate(mode, 0.5, pts, samples);
            for (size_t i = 0; i < samples; ++i) {
                const double t = static_cast<double>(i) / static_cast<double>(samples - 1);
                const Eigen::VectorXd expected = Kinesis::interpolate(matrix, t, mode, 0.5);
                for (size_t d = 0; d < 3; ++d) {
                    EXPECT_NEAR(c.at(d, i), expected(static_cast<Eigen::Index>(d)), TOL)
                        << "sample " << i << " component " << d;
                }
            }
        }
    }

    TEST_F(CurveEvaluatorTest, SingleSampleCatmullRomHitsTheInnerControlsAndTheClassicMidpoint)
    {
        const auto pts = four_points();
        const auto m = as_columns(pts);

        expect_vector(Kinesis::catmull_rom_spline(m, 0.0), pts[1]);
        expect_vector(Kinesis::catmull_rom_spline(m, 1.0), pts[2]);
        expect_vector(Kinesis::catmull_rom_spline(m, 0.5),
            combine({ { -1.0 / 16.0, pts[0] }, { 9.0 / 16.0, pts[1] }, { 9.0 / 16.0, pts[2] }, { -1.0 / 16.0, pts[3] } }));
    }

    TEST_F(CurveEvaluatorTest, SingleSampleCatmullRomTensionZeroIsTheInnerAverage)
    {
        const auto pts = four_points();

        expect_vector(Kinesis::catmull_rom_spline(as_columns(pts), 0.5, 0.0),
            combine({ { 0.5, pts[1] }, { 0.5, pts[2] } }));
    }

    TEST_F(CurveEvaluatorTest, SingleSampleBSplineFollowsTheUniformCubicWeights)
    {
        const auto pts = four_points();
        const auto m = as_columns(pts);

        expect_vector(Kinesis::bspline_cubic(m, 0.0),
            combine({ { 1.0 / 6.0, pts[0] }, { 4.0 / 6.0, pts[1] }, { 1.0 / 6.0, pts[2] } }));
        expect_vector(Kinesis::bspline_cubic(m, 1.0),
            combine({ { 1.0 / 6.0, pts[1] }, { 4.0 / 6.0, pts[2] }, { 1.0 / 6.0, pts[3] } }));
    }

    TEST_F(CurveEvaluatorTest, SingleSampleBeziersInterpolateTheirEnds)
    {
        const auto pts = four_points();
        const auto cubic = as_columns(pts);
        const auto quad = as_columns({ pts[0], pts[1], pts[2] });

        expect_vector(Kinesis::cubic_bezier(cubic, 0.0), pts[0]);
        expect_vector(Kinesis::cubic_bezier(cubic, 1.0), pts[3]);
        expect_vector(Kinesis::cubic_bezier(cubic, 0.5),
            combine({ { 1.0 / 8.0, pts[0] }, { 3.0 / 8.0, pts[1] }, { 3.0 / 8.0, pts[2] }, { 1.0 / 8.0, pts[3] } }));
        expect_vector(Kinesis::quadratic_bezier(quad, 0.0), pts[0]);
        expect_vector(Kinesis::quadratic_bezier(quad, 1.0), pts[2]);
        expect_vector(Kinesis::quadratic_bezier(quad, 0.5),
            combine({ { 0.25, pts[0] }, { 0.5, pts[1] }, { 0.25, pts[2] } }));
    }

    TEST_F(CurveEvaluatorTest, SingleSampleHermiteTakesItsEndpointsAndTangents)
    {
        const auto ends = as_columns({ { 0, 0, 0 }, { 1, 1, 1 } });
        const auto tangents = as_columns({ { 2, 0, 0 }, { 0, 2, 0 } });

        expect_vector(Kinesis::cubic_hermite(ends, tangents, 0.0), { 0, 0, 0 });
        expect_vector(Kinesis::cubic_hermite(ends, tangents, 1.0), { 1, 1, 1 });
        expect_vector(Kinesis::cubic_hermite(ends, tangents, 0.5), { 0.75, 0.25, 0.5 });
    }

    class CurveEvaluatorCountTest : public ::testing::TestWithParam<std::tuple<size_t, Eigen::Index>> {
    protected:
        [[nodiscard]] size_t controls() const { return std::get<0>(GetParam()); }
        [[nodiscard]] Eigen::Index samples() const { return std::get<1>(GetParam()); }
    };

    TEST_P(CurveEvaluatorCountTest, OutputsHoldThreeRowsOfOneValuePerSample)
    {
        const auto c = evaluate(InterpolationMode::CATMULL_ROM, 0.5, line_points(controls()), samples());

        EXPECT_EQ(c.out.size(), 3U * c.samples);
        EXPECT_EQ(c.tangents.size(), 3U * c.samples);
    }

    TEST_P(CurveEvaluatorCountTest, NoSampleSitsAtTheOriginOrHasAZeroComponent)
    {
        const auto c = evaluate(InterpolationMode::CATMULL_ROM, 0.5, positive_random_points(controls()), samples());

        for (size_t i = 0; i < c.samples; ++i) {
            for (size_t d = 0; d < 3; ++d) {
                ASSERT_NE(c.at(d, i), 0.0) << "sample " << i << " component " << d;
                EXPECT_GT(c.at(d, i), 0.5) << "sample " << i << " component " << d;
                EXPECT_LT(c.at(d, i), 2.5) << "sample " << i << " component " << d;
            }
        }
    }

    TEST_P(CurveEvaluatorCountTest, TheCurveEndsWhereTheControlsSayItShould)
    {
        const auto pts = positive_random_points(controls());
        const auto c = evaluate(InterpolationMode::CATMULL_ROM, 0.5, pts, samples());

        if (controls() == 4) {
            expect_sample(c, 0, pts[1]);
            expect_sample(c, c.samples - 1, pts[2]);
        } else {
            expect_sample(c, 0, pts.front());
            expect_sample(c, c.samples - 1, pts.back());
        }
    }

    TEST_P(CurveEvaluatorCountTest, ACollinearPathNeverDoublesBack)
    {
        const auto c = evaluate(InterpolationMode::CATMULL_ROM, 0.5, line_points(controls()), samples());

        for (size_t d = 0; d < 3; ++d) {
            for (size_t i = 1; i < c.samples; ++i) {
                ASSERT_GE(c.at(d, i), c.at(d, i - 1) - TOL) << "sample " << i << " component " << d;
            }
        }
    }

    TEST_P(CurveEvaluatorCountTest, InteriorSegmentsOfAnEvenlySpacedLineAreExactlyLinear)
    {
        const size_t n = controls();
        if (n < 5) {
            GTEST_SKIP() << "a four control path has no interior segment";
        }

        const auto c = evaluate(InterpolationMode::CATMULL_ROM, 0.5, line_points(n), samples());
        const auto span = static_cast<double>(c.samples - 1);

        for (size_t i = 0; i + 1 < c.samples; ++i) {
            const double u = (static_cast<double>(i) / span) * static_cast<double>(n - 1);
            const auto segment = static_cast<size_t>(std::floor(u));
            if (segment < 1 || segment + 3 > n) {
                continue;
            }
            expect_sample(c, i, { 1.0 + u, 1.0 + 2.0 * u, 1.0 + 3.0 * u }, 1e-8);
        }
    }

    INSTANTIATE_TEST_SUITE_P(
        CurveCounts,
        CurveEvaluatorCountTest,
        ::testing::Combine(
            ::testing::Values(size_t { 4 }, size_t { 5 }, size_t { 70 }),
            ::testing::Values(Eigen::Index { 2 }, Eigen::Index { 3 }, Eigen::Index { 10 }, Eigen::Index { 77 },
                Eigen::Index { 500 }, Eigen::Index { 20000 })));

} // namespace

}
