#include <mutex>
#include <base/bit_cast.h>

#include <Common/FieldVisitorDump.h>
#include <Common/FieldVisitorConvertToNumber.h>
#include <DataTypes/DataTypeArray.h>
#include <Columns/ColumnString.h>
#include <Columns/ColumnArray.h>
#include <Columns/ColumnConst.h>
#include <Columns/ColumnsNumber.h>
#include <Columns/ColumnDecimal.h>
#include <Common/Arena.h>
#include <Common/HashTable/HashMap.h>
#include <Common/typeid_cast.h>
#include <base/StringRef.h>
#include <Functions/IFunction.h>
#include <Functions/FunctionHelpers.h>
#include <Functions/FunctionFactory.h>
#include <DataTypes/getLeastSupertype.h>
#include <Interpreters/convertFieldToType.h>


namespace DB
{
namespace ErrorCodes
{
    extern const int ILLEGAL_TYPE_OF_ARGUMENT;
    extern const int BAD_ARGUMENTS;
    extern const int NUMBER_OF_ARGUMENTS_DOESNT_MATCH;
    extern const int ILLEGAL_COLUMN;
}

namespace
{

/** transform(x, from_array, to_array[, default]) - convert x according to an explicitly passed match.
  */

/** transform(x, [from...], [to...], default)
  * - converts the values according to the explicitly specified mapping.
  *
  * x - what to transform.
  * from - a constant array of values for the transformation.
  * to - a constant array of values into which values from `from` must be transformed.
  * default - what value to use if x is not equal to any of the values in `from`.
  * `from` and `to` - arrays of the same size.
  *
  * Types:
  * transform(T, Array(T), Array(U), U) -> U
  *
  * transform(x, [from...], [to...])
  * - if `default` is not specified, then for values of `x` for which there is no corresponding element in `from`, the unchanged value of `x` is returned.
  *
  * Types:
  * transform(T, Array(T), Array(T)) -> T
  *
  * Note: the implementation is rather cumbersome.
  */
class FunctionTransform : public IFunction
{
public:
    static constexpr auto name = "transform";
    static FunctionPtr create(ContextPtr) { return std::make_shared<FunctionTransform>(); }

    String getName() const override
    {
        return name;
    }

    bool isVariadic() const override { return true; }
    bool isSuitableForShortCircuitArgumentsExecution(const DataTypesWithConstInfo & /*arguments*/) const override { return true; }
    size_t getNumberOfArguments() const override { return 0; }
    bool useDefaultImplementationForConstants() const override { return true; }
    ColumnNumbers getArgumentsThatAreAlwaysConstant() const override { return {1, 2}; }

    DataTypePtr getReturnTypeImpl(const DataTypes & arguments) const override
    {
        const auto args_size = arguments.size();
        if (args_size != 3 && args_size != 4)
            throw Exception(ErrorCodes::NUMBER_OF_ARGUMENTS_DOESNT_MATCH, "Number of arguments for function {} doesn't match: "
                "passed {}, should be 3 or 4", getName(), args_size);

        const DataTypePtr & type_x = arguments[0];

        if (!type_x->isValueRepresentedByNumber() && !isString(type_x))
            throw Exception(ErrorCodes::ILLEGAL_TYPE_OF_ARGUMENT,
                            "Unsupported type {} of first argument "
                            "of function {}, must be numeric type or Date/DateTime or String",
                            type_x->getName(), getName());

        const DataTypeArray * type_arr_from = checkAndGetDataType<DataTypeArray>(arguments[1].get());

        if (!type_arr_from)
            throw Exception(ErrorCodes::ILLEGAL_TYPE_OF_ARGUMENT,
                            "Second argument of function {}, must be array of source values to transform from.",
                            getName());

        const auto type_arr_from_nested = type_arr_from->getNestedType();

        if ((type_x->isValueRepresentedByNumber() != type_arr_from_nested->isValueRepresentedByNumber())
            || (isString(type_x) != isString(type_arr_from_nested)))
        {
            throw Exception(ErrorCodes::ILLEGAL_TYPE_OF_ARGUMENT,
                            "First argument and elements of array "
                            "of second argument of function {} must have compatible types: "
                            "both numeric or both strings.", getName());
        }

        const DataTypeArray * type_arr_to = checkAndGetDataType<DataTypeArray>(arguments[2].get());

        if (!type_arr_to)
            throw Exception(ErrorCodes::ILLEGAL_TYPE_OF_ARGUMENT,
                            "Third argument of function {}, must be array of destination values to transform to.",
                            getName());

        const DataTypePtr & type_arr_to_nested = type_arr_to->getNestedType();

        if (args_size == 3)
        {
            if ((type_x->isValueRepresentedByNumber() != type_arr_to_nested->isValueRepresentedByNumber())
                || (isString(type_x) != isString(type_arr_to_nested)))
                throw Exception(ErrorCodes::ILLEGAL_TYPE_OF_ARGUMENT, "Function {} has signature: "
                                "transform(T, Array(T), Array(U), U) -> U; "
                                "or transform(T, Array(T), Array(T)) -> T; where T and U are types.", getName());

            return getLeastSupertype(DataTypes{type_x, type_arr_to_nested});
        }
        else
        {
            const DataTypePtr & type_default = arguments[3];

            if (!type_default->isValueRepresentedByNumber() && !isString(type_default))
                throw Exception(ErrorCodes::ILLEGAL_TYPE_OF_ARGUMENT,
                                "Unsupported type {} of fourth argument (default value) "
                                "of function {}, must be numeric type or Date/DateTime or String",
                                type_default->getName(), getName());

            bool default_is_string = WhichDataType(type_default).isString();
            bool nested_is_string = WhichDataType(type_arr_to_nested).isString();

            if ((type_default->isValueRepresentedByNumber() != type_arr_to_nested->isValueRepresentedByNumber())
                || (default_is_string != nested_is_string))
                throw Exception(ErrorCodes::ILLEGAL_TYPE_OF_ARGUMENT, "Function {} have signature: "
                                "transform(T, Array(T), Array(U), U) -> U; "
                                "or transform(T, Array(T), Array(T)) -> T; where T and U are types.", getName());

            if (type_arr_to_nested->isValueRepresentedByNumber() && type_default->isValueRepresentedByNumber())
            {
                /// We take the smallest common type for the elements of the array of values `to` and for `default`.
                return getLeastSupertype(DataTypes{type_arr_to_nested, type_default});
            }

            /// TODO More checks.
            return type_arr_to_nested;
        }
    }

    ColumnPtr executeImpl(const ColumnsWithTypeAndName & arguments, const DataTypePtr & result_type, size_t input_rows_count) const override
    {
        const ColumnConst * array_from = checkAndGetColumnConst<ColumnArray>(arguments[1].column.get());
        const ColumnConst * array_to = checkAndGetColumnConst<ColumnArray>(arguments[2].column.get());

        if (!array_from || !array_to)
            throw Exception(ErrorCodes::ILLEGAL_COLUMN, "Second and third arguments of function {} must be constant arrays.", getName());

        initialize(array_from->getValue<Array>(), array_to->getValue<Array>(), arguments);

        const auto * in = arguments.front().column.get();

        if (isColumnConst(*in))
            return executeConst(arguments, result_type, input_rows_count);

        const IColumn * default_column = nullptr;
        if (arguments.size() == 4)
            default_column = arguments[3].column.get();

        auto column_result = result_type->createColumn();
        auto * out = column_result.get();

        if (!executeNum<UInt8>(in, out, default_column)
            && !executeNum<UInt16>(in, out, default_column)
            && !executeNum<UInt32>(in, out, default_column)
            && !executeNum<UInt64>(in, out, default_column)
            && !executeNum<Int8>(in, out, default_column)
            && !executeNum<Int16>(in, out, default_column)
            && !executeNum<Int32>(in, out, default_column)
            && !executeNum<Int64>(in, out, default_column)
            && !executeNum<Float32>(in, out, default_column)
            && !executeNum<Float64>(in, out, default_column)
            && !executeDecimal<Decimal32>(in, out, default_column)
            && !executeDecimal<Decimal64>(in, out, default_column)
            && !executeString(in, out, default_column))
        {
            throw Exception(ErrorCodes::ILLEGAL_COLUMN, "Illegal column {} of first argument of function {}", in->getName(), getName());
        }

        return column_result;
    }

private:
    static ColumnPtr executeConst(const ColumnsWithTypeAndName & arguments, const DataTypePtr & result_type, size_t input_rows_count)
    {
        /// Materialize the input column and compute the function as usual.

        ColumnsWithTypeAndName args = arguments;
        args[0].column = args[0].column->cloneResized(input_rows_count)->convertToFullColumnIfConst();

        auto impl = FunctionToOverloadResolverAdaptor(std::make_shared<FunctionTransform>()).build(args);

        return impl->execute(args, result_type, input_rows_count);
    }

    template <typename T>
    bool executeNum(const IColumn * in_untyped, IColumn * out_untyped, const IColumn * default_untyped) const
    {
        if (const auto in = checkAndGetColumn<ColumnVector<T>>(in_untyped))
        {
            if (!default_untyped)
            {
                auto out = typeid_cast<ColumnVector<T> *>(out_untyped);
                if (!out)
                {
                    throw Exception(ErrorCodes::ILLEGAL_COLUMN,
                                    "Illegal column {} of elements "
                                    "of array of third argument of function {}, must be {}",
                                    out_untyped->getName(), getName(), in->getName());
                }

                executeImplNumToNum<T>(in->getData(), out->getData());
            }
            else if (isColumnConst(*default_untyped))
            {
                if (!executeNumToNumWithConstDefault<T, UInt8>(in, out_untyped)
                    && !executeNumToNumWithConstDefault<T, UInt16>(in, out_untyped)
                    && !executeNumToNumWithConstDefault<T, UInt32>(in, out_untyped)
                    && !executeNumToNumWithConstDefault<T, UInt64>(in, out_untyped)
                    && !executeNumToNumWithConstDefault<T, Int8>(in, out_untyped)
                    && !executeNumToNumWithConstDefault<T, Int16>(in, out_untyped)
                    && !executeNumToNumWithConstDefault<T, Int32>(in, out_untyped)
                    && !executeNumToNumWithConstDefault<T, Int64>(in, out_untyped)
                    && !executeNumToNumWithConstDefault<T, Float32>(in, out_untyped)
                    && !executeNumToNumWithConstDefault<T, Float64>(in, out_untyped)
                    && !executeNumToDecimalWithConstDefault<T, Decimal32>(in, out_untyped)
                    && !executeNumToDecimalWithConstDefault<T, Decimal64>(in, out_untyped)
                    && !executeNumToStringWithConstDefault<T>(in, out_untyped))
                {
                    throw Exception(ErrorCodes::ILLEGAL_COLUMN, "Illegal column {} of elements of array of second argument of function {}",
                        in->getName(), getName());
                }
            }
            else
            {
                if (!executeNumToNumWithNonConstDefault<T, UInt8>(in, out_untyped, default_untyped)
                    && !executeNumToNumWithNonConstDefault<T, UInt16>(in, out_untyped, default_untyped)
                    && !executeNumToNumWithNonConstDefault<T, UInt32>(in, out_untyped, default_untyped)
                    && !executeNumToNumWithNonConstDefault<T, UInt64>(in, out_untyped, default_untyped)
                    && !executeNumToNumWithNonConstDefault<T, Int8>(in, out_untyped, default_untyped)
                    && !executeNumToNumWithNonConstDefault<T, Int16>(in, out_untyped, default_untyped)
                    && !executeNumToNumWithNonConstDefault<T, Int32>(in, out_untyped, default_untyped)
                    && !executeNumToNumWithNonConstDefault<T, Int64>(in, out_untyped, default_untyped)
                    && !executeNumToNumWithNonConstDefault<T, Float32>(in, out_untyped, default_untyped)
                    && !executeNumToNumWithNonConstDefault<T, Float64>(in, out_untyped, default_untyped)
                    && !executeNumToDecimalWithNonConstDefault<T, Decimal32>(in, out_untyped, default_untyped)
                    && !executeNumToDecimalWithNonConstDefault<T, Decimal64>(in, out_untyped, default_untyped)
                    && !executeNumToStringWithNonConstDefault<T>(in, out_untyped, default_untyped))
                {
                    throw Exception(ErrorCodes::ILLEGAL_COLUMN, "Illegal column {} of elements of array of second argument of function {}",
                        in->getName(), getName());
                }
            }

            return true;
        }

        return false;
    }

    template <typename T>
    bool executeDecimal(const IColumn * in_untyped, IColumn * out_untyped, const IColumn * default_untyped) const
    {
        if (const auto in = checkAndGetColumn<ColumnDecimal<T>>(in_untyped))
        {
            if (!default_untyped)
            {
                auto out = typeid_cast<ColumnDecimal<T> *>(out_untyped);
                if (!out)
                {
                    throw Exception(ErrorCodes::ILLEGAL_COLUMN,
                                    "Illegal column {} of elements "
                                    "of array of third argument of function {}, must be {}",
                                    out_untyped->getName(), getName(), in->getName());
                }

                executeImplNumToNum<T>(in->getData(), out->getData());
            }
            else if (isColumnConst(*default_untyped))
            {
                if (!executeDecimalToNumWithConstDefault<T, UInt8>(in, out_untyped)
                    && !executeDecimalToNumWithConstDefault<T, UInt16>(in, out_untyped)
                    && !executeDecimalToNumWithConstDefault<T, UInt32>(in, out_untyped)
                    && !executeDecimalToNumWithConstDefault<T, UInt64>(in, out_untyped)
                    && !executeDecimalToNumWithConstDefault<T, Int8>(in, out_untyped)
                    && !executeDecimalToNumWithConstDefault<T, Int16>(in, out_untyped)
                    && !executeDecimalToNumWithConstDefault<T, Int32>(in, out_untyped)
                    && !executeDecimalToNumWithConstDefault<T, Int64>(in, out_untyped)
                    && !executeDecimalToNumWithConstDefault<T, Float32>(in, out_untyped)
                    && !executeDecimalToNumWithConstDefault<T, Float64>(in, out_untyped)
                    && !executeDecimalToDecimalWithConstDefault<T, Decimal32>(in, out_untyped)
                    && !executeDecimalToDecimalWithConstDefault<T, Decimal64>(in, out_untyped)
                    && !executeDecimalToStringWithConstDefault<T>(in, out_untyped))
                {
                    throw Exception(ErrorCodes::ILLEGAL_COLUMN, "Illegal column {} of elements of array of second argument of function {}",
                                    in->getName(), getName());
                }
            }
            else
            {
                if (!executeDecimalToNumWithNonConstDefault<T, UInt8>(in, out_untyped, default_untyped)
                    && !executeDecimalToNumWithNonConstDefault<T, UInt16>(in, out_untyped, default_untyped)
                    && !executeDecimalToNumWithNonConstDefault<T, UInt32>(in, out_untyped, default_untyped)
                    && !executeDecimalToNumWithNonConstDefault<T, UInt64>(in, out_untyped, default_untyped)
                    && !executeDecimalToNumWithNonConstDefault<T, Int8>(in, out_untyped, default_untyped)
                    && !executeDecimalToNumWithNonConstDefault<T, Int16>(in, out_untyped, default_untyped)
                    && !executeDecimalToNumWithNonConstDefault<T, Int32>(in, out_untyped, default_untyped)
                    && !executeDecimalToNumWithNonConstDefault<T, Int64>(in, out_untyped, default_untyped)
                    && !executeDecimalToNumWithNonConstDefault<T, Float32>(in, out_untyped, default_untyped)
                    && !executeDecimalToNumWithNonConstDefault<T, Float64>(in, out_untyped, default_untyped)
                    && !executeDecimalToDecimalWithNonConstDefault<T, Decimal32>(in, out_untyped, default_untyped)
                    && !executeDecimalToDecimalWithNonConstDefault<T, Decimal64>(in, out_untyped, default_untyped)
                    && !executeDecimalToStringWithNonConstDefault<T>(in, out_untyped, default_untyped))
                {
                    throw Exception(ErrorCodes::ILLEGAL_COLUMN, "Illegal column {} of elements of array of second argument of function {}",
                                    in->getName(), getName());
                }
            }

            return true;
        }

        return false;
    }

    bool executeString(const IColumn * in_untyped, IColumn * out_untyped, const IColumn * default_untyped) const
    {
        if (const auto * in = checkAndGetColumn<ColumnString>(in_untyped))
        {
            if (!default_untyped)
            {
                if (!executeStringToString(in, out_untyped))
                    throw Exception(ErrorCodes::ILLEGAL_COLUMN, "Illegal column {} of elements of array of second argument of function {}",
                        in->getName(), getName());
            }
            else if (isColumnConst(*default_untyped))
            {
                if (!executeStringToNumWithConstDefault<UInt8>(in, out_untyped)
                    && !executeStringToNumWithConstDefault<UInt16>(in, out_untyped)
                    && !executeStringToNumWithConstDefault<UInt32>(in, out_untyped)
                    && !executeStringToNumWithConstDefault<UInt64>(in, out_untyped)
                    && !executeStringToNumWithConstDefault<Int8>(in, out_untyped)
                    && !executeStringToNumWithConstDefault<Int16>(in, out_untyped)
                    && !executeStringToNumWithConstDefault<Int32>(in, out_untyped)
                    && !executeStringToNumWithConstDefault<Int64>(in, out_untyped)
                    && !executeStringToNumWithConstDefault<Float32>(in, out_untyped)
                    && !executeStringToNumWithConstDefault<Float64>(in, out_untyped)
                    && !executeStringToDecimalWithConstDefault<Decimal32>(in, out_untyped)
                    && !executeStringToDecimalWithConstDefault<Decimal64>(in, out_untyped)
                    && !executeStringToStringWithConstDefault(in, out_untyped))
                {
                    throw Exception(ErrorCodes::ILLEGAL_COLUMN, "Illegal column {} of elements of array of second argument of function {}",
                        in->getName(), getName());
                }
            }
            else
            {
                if (!executeStringToNumWithNonConstDefault<UInt8>(in, out_untyped, default_untyped)
                    && !executeStringToNumWithNonConstDefault<UInt16>(in, out_untyped, default_untyped)
                    && !executeStringToNumWithNonConstDefault<UInt32>(in, out_untyped, default_untyped)
                    && !executeStringToNumWithNonConstDefault<UInt64>(in, out_untyped, default_untyped)
                    && !executeStringToNumWithNonConstDefault<Int8>(in, out_untyped, default_untyped)
                    && !executeStringToNumWithNonConstDefault<Int16>(in, out_untyped, default_untyped)
                    && !executeStringToNumWithNonConstDefault<Int32>(in, out_untyped, default_untyped)
                    && !executeStringToNumWithNonConstDefault<Int64>(in, out_untyped, default_untyped)
                    && !executeStringToNumWithNonConstDefault<Float32>(in, out_untyped, default_untyped)
                    && !executeStringToNumWithNonConstDefault<Float64>(in, out_untyped, default_untyped)
                    && !executeStringToDecimalWithNonConstDefault<Decimal32>(in, out_untyped, default_untyped)
                    && !executeStringToDecimalWithNonConstDefault<Decimal64>(in, out_untyped, default_untyped)

                    && !executeStringToStringWithNonConstDefault(in, out_untyped, default_untyped))
                {
                    throw Exception(ErrorCodes::ILLEGAL_COLUMN, "Illegal column {} of elements of array of second argument of function {}",
                        in->getName(), getName());
                }
            }

            return true;
        }

        return false;
    }

    template <typename T, typename U>
    bool executeNumToNumWithConstDefault(const ColumnVector<T> * in, IColumn * out_untyped) const
    {
        auto out = typeid_cast<ColumnVector<U> *>(out_untyped);
        if (!out)
            return false;

        executeImplNumToNumWithConstDefault<T, U>(in->getData(), out->getData(), static_cast<U>(cache.const_default_value.get<U>()));
        return true;
    }

    template <typename T, typename U>
    bool executeNumToDecimalWithConstDefault(const ColumnVector<T> * in, IColumn * out_untyped) const
    {
        auto out = typeid_cast<ColumnDecimal<U> *>(out_untyped);
        if (!out)
            return false;

        executeImplNumToNumWithConstDefault<T, U>(in->getData(), out->getData(), cache.const_default_value.get<U>());
        return true;
    }


    template <typename T, typename U>
    bool executeDecimalToNumWithConstDefault(const ColumnDecimal<T> * in, IColumn * out_untyped) const
    {
        auto out = typeid_cast<ColumnVector<U> *>(out_untyped);
        if (!out)
            return false;

        executeImplNumToNumWithConstDefault<T, U>(in->getData(), out->getData(), static_cast<U>(cache.const_default_value.get<U>()));
        return true;
    }

    template <typename T, typename U>
    bool executeDecimalToDecimalWithConstDefault(const ColumnDecimal<T> * in, IColumn * out_untyped) const
    {
        auto out = typeid_cast<ColumnDecimal<U> *>(out_untyped);
        if (!out)
            return false;

        executeImplNumToNumWithConstDefault<T, U>(in->getData(), out->getData(), cache.const_default_value.get<U>());
        return true;
    }

    template <typename T, typename U>
    bool executeNumToNumWithNonConstDefault(const ColumnVector<T> * in, IColumn * out_untyped, const IColumn * default_untyped) const
    {
        auto out = typeid_cast<ColumnVector<U> *>(out_untyped);
        if (!out)
            return false;

        if (!executeNumToNumWithNonConstDefault2<T, U, UInt8>(in, out, default_untyped)
            && !executeNumToNumWithNonConstDefault2<T, U, UInt16>(in, out, default_untyped)
            && !executeNumToNumWithNonConstDefault2<T, U, UInt32>(in, out, default_untyped)
            && !executeNumToNumWithNonConstDefault2<T, U, UInt64>(in, out, default_untyped)
            && !executeNumToNumWithNonConstDefault2<T, U, Int8>(in, out, default_untyped)
            && !executeNumToNumWithNonConstDefault2<T, U, Int16>(in, out, default_untyped)
            && !executeNumToNumWithNonConstDefault2<T, U, Int32>(in, out, default_untyped)
            && !executeNumToNumWithNonConstDefault2<T, U, Int64>(in, out, default_untyped)
            && !executeNumToNumWithNonConstDefault2<T, U, Float32>(in, out, default_untyped)
            && !executeNumToNumWithNonConstDefault2<T, U, Float64>(in, out, default_untyped))
        {
            throw Exception(ErrorCodes::ILLEGAL_COLUMN, "Illegal column {} of fourth argument of function {}",
                default_untyped->getName(), getName());
        }

        return true;
    }

    template <typename T, typename U>
    bool executeNumToDecimalWithNonConstDefault(const ColumnVector<T> * in, IColumn * out_untyped, const IColumn * default_untyped) const
    {
        auto out = typeid_cast<ColumnDecimal<U> *>(out_untyped);
        if (!out)
            return false;

        if (!executeNumToDecimalWithNonConstDefault2<T, U, UInt8>(in, out, default_untyped)
            && !executeNumToDecimalWithNonConstDefault2<T, U, UInt16>(in, out, default_untyped)
            && !executeNumToDecimalWithNonConstDefault2<T, U, UInt32>(in, out, default_untyped)
            && !executeNumToDecimalWithNonConstDefault2<T, U, UInt64>(in, out, default_untyped)
            && !executeNumToDecimalWithNonConstDefault2<T, U, Int8>(in, out, default_untyped)
            && !executeNumToDecimalWithNonConstDefault2<T, U, Int16>(in, out, default_untyped)
            && !executeNumToDecimalWithNonConstDefault2<T, U, Int32>(in, out, default_untyped)
            && !executeNumToDecimalWithNonConstDefault2<T, U, Int64>(in, out, default_untyped)
            && !executeNumToDecimalWithNonConstDefault2<T, U, Float32>(in, out, default_untyped)
            && !executeNumToDecimalWithNonConstDefault2<T, U, Float64>(in, out, default_untyped)
            && !executeNumToDecimalWithNonConstDefaultDecimal2<T, U, Decimal32>(in, out, default_untyped)
            && !executeNumToDecimalWithNonConstDefaultDecimal2<T, U, Decimal64>(in, out, default_untyped))
        {
            throw Exception(ErrorCodes::ILLEGAL_COLUMN, "Illegal column {} of fourth argument of function {}",
                default_untyped->getName(), getName());
        }

        return true;
    }

    template <typename T, typename U>
    bool executeDecimalToNumWithNonConstDefault(const ColumnDecimal<T> * in, IColumn * out_untyped, const IColumn * default_untyped) const
    {
        auto out = typeid_cast<ColumnVector<U> *>(out_untyped);
        if (!out)
            return false;

        if (!executeDecimalToNumWithNonConstDefault2<T, U, UInt8>(in, out, default_untyped)
            && !executeDecimalToNumWithNonConstDefault2<T, U, UInt16>(in, out, default_untyped)
            && !executeDecimalToNumWithNonConstDefault2<T, U, UInt32>(in, out, default_untyped)
            && !executeDecimalToNumWithNonConstDefault2<T, U, UInt64>(in, out, default_untyped)
            && !executeDecimalToNumWithNonConstDefault2<T, U, Int8>(in, out, default_untyped)
            && !executeDecimalToNumWithNonConstDefault2<T, U, Int16>(in, out, default_untyped)
            && !executeDecimalToNumWithNonConstDefault2<T, U, Int32>(in, out, default_untyped)
            && !executeDecimalToNumWithNonConstDefault2<T, U, Int64>(in, out, default_untyped)
            && !executeDecimalToNumWithNonConstDefault2<T, U, Float32>(in, out, default_untyped)
            && !executeDecimalToNumWithNonConstDefault2<T, U, Float64>(in, out, default_untyped)
            && !executeDecimalToNumWithNonConstDefaultDecimal2<T, U, Decimal32>(in, out, default_untyped)
            && !executeDecimalToNumWithNonConstDefaultDecimal2<T, U, Decimal64>(in, out, default_untyped))
        {
            throw Exception(ErrorCodes::ILLEGAL_COLUMN, "Illegal column {} of fourth argument of function {}",
                default_untyped->getName(), getName());
        }

        return true;
    }

    template <typename T, typename U>
    bool executeDecimalToDecimalWithNonConstDefault(const ColumnDecimal<T> * in, IColumn * out_untyped, const IColumn * default_untyped) const
    {
        auto out = typeid_cast<ColumnDecimal<U> *>(out_untyped);
        if (!out)
            return false;

        if (!executeDecimalToDecimalWithNonConstDefault2<T, U, UInt8>(in, out, default_untyped)
            && !executeDecimalToDecimalWithNonConstDefault2<T, U, UInt16>(in, out, default_untyped)
            && !executeDecimalToDecimalWithNonConstDefault2<T, U, UInt32>(in, out, default_untyped)
            && !executeDecimalToDecimalWithNonConstDefault2<T, U, UInt64>(in, out, default_untyped)
            && !executeDecimalToDecimalWithNonConstDefault2<T, U, Int8>(in, out, default_untyped)
            && !executeDecimalToDecimalWithNonConstDefault2<T, U, Int16>(in, out, default_untyped)
            && !executeDecimalToDecimalWithNonConstDefault2<T, U, Int32>(in, out, default_untyped)
            && !executeDecimalToDecimalWithNonConstDefault2<T, U, Int64>(in, out, default_untyped)
            && !executeDecimalToDecimalWithNonConstDefault2<T, U, Float32>(in, out, default_untyped)
            && !executeDecimalToDecimalWithNonConstDefault2<T, U, Float64>(in, out, default_untyped)
            && !executeDecimalToDecimalWithNonConstDefaultDecimal2<T, U, Decimal32>(in, out, default_untyped)
            && !executeDecimalToDecimalWithNonConstDefaultDecimal2<T, U, Decimal64>(in, out, default_untyped))
        {
            throw Exception(ErrorCodes::ILLEGAL_COLUMN, "Illegal column {} of fourth argument of function {}",
                default_untyped->getName(), getName());
        }

        return true;
    }

    template <typename T, typename U, typename V>
    bool executeNumToNumWithNonConstDefault2(const ColumnVector<T> * in, ColumnVector<U> * out, const IColumn * default_untyped) const
    {
        auto col_default = checkAndGetColumn<ColumnVector<V>>(default_untyped);
        if (!col_default)
            return false;

        executeImplNumToNumWithNonConstDefault<T, U, V>(in->getData(), out->getData(), col_default->getData());
        return true;
    }

    template <typename T, typename U, typename V>
    bool executeNumToDecimalWithNonConstDefault2(const ColumnVector<T> * in, ColumnDecimal<U> * out, const IColumn * default_untyped) const
    {
        auto col_default = checkAndGetColumn<ColumnVector<V>>(default_untyped);
        if (!col_default)
            return false;

        executeImplNumToNumWithNonConstDefault<T, U, V>(in->getData(), out->getData(), col_default->getData());
        return true;
    }

    template <typename T, typename U, typename V>
    bool executeNumToDecimalWithNonConstDefaultDecimal2(const ColumnVector<T> * in, ColumnDecimal<U> * out, const IColumn * default_untyped) const
    {
        auto col_default = checkAndGetColumn<ColumnDecimal<V>>(default_untyped);
        if (!col_default)
            return false;

        executeImplNumToNumWithNonConstDefault<T, U, V>(in->getData(), out->getData(), col_default->getData());
        return true;
    }

    template <typename T, typename U, typename V>
    bool executeDecimalToNumWithNonConstDefault2(const ColumnDecimal<T> * in, ColumnVector<U> * out, const IColumn * default_untyped) const
    {
        auto col_default = checkAndGetColumn<ColumnVector<V>>(default_untyped);
        if (!col_default)
            return false;

        executeImplNumToNumWithNonConstDefault<T, U, V>(in->getData(), out->getData(), col_default->getData());
        return true;
    }

    template <typename T, typename U, typename V>
    bool executeDecimalToDecimalWithNonConstDefault2(const ColumnDecimal<T> * in, ColumnDecimal<U> * out, const IColumn * default_untyped) const
    {
        auto col_default = checkAndGetColumn<ColumnVector<V>>(default_untyped);
        if (!col_default)
            return false;

        executeImplNumToNumWithNonConstDefault<T, U, V>(in->getData(), out->getData(), col_default->getData());
        return true;
    }

    template <typename T, typename U, typename V>
    bool executeDecimalToNumWithNonConstDefaultDecimal2(const ColumnDecimal<T> * in, ColumnVector<U> * out, const IColumn * default_untyped) const
    {
        auto col_default = checkAndGetColumn<ColumnDecimal<V>>(default_untyped);
        if (!col_default)
            return false;

        executeImplNumToNumWithNonConstDefault<T, U, V>(in->getData(), out->getData(), col_default->getData());
        return true;
    }

    template <typename T, typename U, typename V>
    bool executeDecimalToDecimalWithNonConstDefaultDecimal2(const ColumnDecimal<T> * in, ColumnDecimal<U> * out, const IColumn * default_untyped) const
    {
        auto col_default = checkAndGetColumn<ColumnDecimal<V>>(default_untyped);
        if (!col_default)
            return false;

        executeImplNumToNumWithNonConstDefault<T, U, V>(in->getData(), out->getData(), col_default->getData());
        return true;
    }

    template <typename T>
    bool executeNumToStringWithConstDefault(const ColumnVector<T> * in, IColumn * out_untyped) const
    {
        auto * out = typeid_cast<ColumnString *>(out_untyped);
        if (!out)
            return false;

        const String & default_str = cache.const_default_value.get<const String &>();
        StringRef default_string_ref{default_str.data(), default_str.size() + 1};
        executeImplNumToStringWithConstDefault<T>(in->getData(), out->getChars(), out->getOffsets(), default_string_ref);
        return true;
    }

    template <typename T>
    bool executeDecimalToStringWithConstDefault(const ColumnDecimal<T> * in, IColumn * out_untyped) const
    {
        auto * out = typeid_cast<ColumnString *>(out_untyped);
        if (!out)
            return false;

        const String & default_str = cache.const_default_value.get<const String &>();
        StringRef default_string_ref{default_str.data(), default_str.size() + 1};
        executeImplNumToStringWithConstDefault<T>(in->getData(), out->getChars(), out->getOffsets(), default_string_ref);
        return true;
    }

    template <typename T>
    bool executeNumToStringWithNonConstDefault(const ColumnVector<T> * in, IColumn * out_untyped, const IColumn * default_untyped) const
    {
        auto * out = typeid_cast<ColumnString *>(out_untyped);
        if (!out)
            return false;

        const auto * default_col = checkAndGetColumn<ColumnString>(default_untyped);
        if (!default_col)
        {
            throw Exception(ErrorCodes::ILLEGAL_COLUMN, "Illegal column {} of fourth argument of function {}",
                default_untyped->getName(), getName());
        }

        executeImplNumToStringWithNonConstDefault<T>(
            in->getData(),
            out->getChars(), out->getOffsets(),
            default_col->getChars(), default_col->getOffsets());

        return true;
    }

    template <typename T>
    bool executeDecimalToStringWithNonConstDefault(const ColumnDecimal<T> * in, IColumn * out_untyped, const IColumn * default_untyped) const
    {
        auto * out = typeid_cast<ColumnString *>(out_untyped);
        if (!out)
            return false;

        const auto * default_col = checkAndGetColumn<ColumnString>(default_untyped);
        if (!default_col)
        {
            throw Exception(ErrorCodes::ILLEGAL_COLUMN, "Illegal column {} of fourth argument of function {}",
                            default_untyped->getName(), getName());
        }

        executeImplNumToStringWithNonConstDefault<T>(
            in->getData(),
            out->getChars(), out->getOffsets(),
            default_col->getChars(), default_col->getOffsets());

        return true;
    }

    template <typename U>
    bool executeStringToNumWithConstDefault(const ColumnString * in, IColumn * out_untyped) const
    {
        auto out = typeid_cast<ColumnVector<U> *>(out_untyped);
        if (!out)
            return false;

        executeImplStringToNumWithConstDefault<U>(
            in->getChars(), in->getOffsets(), out->getData(), static_cast<U>(cache.const_default_value.get<U>()));
        return true;
    }

    template <typename U>
    bool executeStringToDecimalWithConstDefault(const ColumnString * in, IColumn * out_untyped) const
    {
        auto out = typeid_cast<ColumnDecimal<U> *>(out_untyped);
        if (!out)
            return false;

        executeImplStringToNumWithConstDefault<U>(in->getChars(), in->getOffsets(), out->getData(), cache.const_default_value.get<U>());
        return true;
    }

    template <typename U>
    bool executeStringToNumWithNonConstDefault(const ColumnString * in, IColumn * out_untyped, const IColumn * default_untyped) const
    {
        auto out = typeid_cast<ColumnVector<U> *>(out_untyped);
        if (!out)
            return false;

        if (!executeStringToNumWithNonConstDefault2<U, UInt8>(in, out, default_untyped)
            && !executeStringToNumWithNonConstDefault2<U, UInt16>(in, out, default_untyped)
            && !executeStringToNumWithNonConstDefault2<U, UInt32>(in, out, default_untyped)
            && !executeStringToNumWithNonConstDefault2<U, UInt64>(in, out, default_untyped)
            && !executeStringToNumWithNonConstDefault2<U, Int8>(in, out, default_untyped)
            && !executeStringToNumWithNonConstDefault2<U, Int16>(in, out, default_untyped)
            && !executeStringToNumWithNonConstDefault2<U, Int32>(in, out, default_untyped)
            && !executeStringToNumWithNonConstDefault2<U, Int64>(in, out, default_untyped)
            && !executeStringToNumWithNonConstDefault2<U, Float32>(in, out, default_untyped)
            && !executeStringToNumWithNonConstDefault2<U, Float64>(in, out, default_untyped))
        {
            throw Exception(ErrorCodes::ILLEGAL_COLUMN, "Illegal column {} of fourth argument of function {}",
                default_untyped->getName(), getName());
        }

        return true;
    }

    template <typename U>
    bool executeStringToDecimalWithNonConstDefault(const ColumnString * in, IColumn * out_untyped, const IColumn * default_untyped) const
    {
        auto out = typeid_cast<ColumnDecimal<U> *>(out_untyped);
        if (!out)
            return false;

        if (!executeStringToDecimalWithNonConstDefault2<U, UInt8>(in, out, default_untyped)
            && !executeStringToDecimalWithNonConstDefault2<U, UInt16>(in, out, default_untyped)
            && !executeStringToDecimalWithNonConstDefault2<U, UInt32>(in, out, default_untyped)
            && !executeStringToDecimalWithNonConstDefault2<U, UInt64>(in, out, default_untyped)
            && !executeStringToDecimalWithNonConstDefault2<U, Int8>(in, out, default_untyped)
            && !executeStringToDecimalWithNonConstDefault2<U, Int16>(in, out, default_untyped)
            && !executeStringToDecimalWithNonConstDefault2<U, Int32>(in, out, default_untyped)
            && !executeStringToDecimalWithNonConstDefault2<U, Int64>(in, out, default_untyped)
            && !executeStringToDecimalWithNonConstDefault2<U, Float32>(in, out, default_untyped)
            && !executeStringToDecimalWithNonConstDefault2<U, Float64>(in, out, default_untyped)
            && !executeStringToDecimalWithNonConstDefaultDecimal2<U, Decimal32>(in, out, default_untyped)
            && !executeStringToDecimalWithNonConstDefaultDecimal2<U, Decimal64>(in, out, default_untyped))
        {
            throw Exception(ErrorCodes::ILLEGAL_COLUMN, "Illegal column {} of fourth argument of function {}",
                            default_untyped->getName(), getName());
        }

        return true;
    }


    template <typename U, typename V>
    bool executeStringToNumWithNonConstDefault2(const ColumnString * in, ColumnVector<U> * out, const IColumn * default_untyped) const
    {
        auto col_default = checkAndGetColumn<ColumnVector<V>>(default_untyped);
        if (!col_default)
            return false;

        executeImplStringToNumWithNonConstDefault<U, V>(in->getChars(), in->getOffsets(), out->getData(), col_default->getData());
        return true;
    }

    template <typename U, typename V>
    bool executeStringToDecimalWithNonConstDefault2(const ColumnString * in, ColumnDecimal<U> * out, const IColumn * default_untyped) const
    {
        auto col_default = checkAndGetColumn<ColumnVector<V>>(default_untyped);
        if (!col_default)
            return false;

        executeImplStringToNumWithNonConstDefault<U, V>(in->getChars(), in->getOffsets(), out->getData(), col_default->getData());
        return true;
    }

    template <typename U, typename V>
    bool executeStringToDecimalWithNonConstDefaultDecimal2(const ColumnString * in, ColumnDecimal<U> * out, const IColumn * default_untyped) const
    {
        auto col_default = checkAndGetColumn<ColumnDecimal<V>>(default_untyped);
        if (!col_default)
            return false;

        executeImplStringToNumWithNonConstDefault<U, V>(in->getChars(), in->getOffsets(), out->getData(), col_default->getData());
        return true;
    }

    bool executeStringToString(const ColumnString * in, IColumn * out_untyped) const
    {
        auto * out = typeid_cast<ColumnString *>(out_untyped);
        if (!out)
            return false;

        executeImplStringToString(in->getChars(), in->getOffsets(), out->getChars(), out->getOffsets());
        return true;
    }

    bool executeStringToStringWithConstDefault(const ColumnString * in, IColumn * out_untyped) const
    {
        auto * out = typeid_cast<ColumnString *>(out_untyped);
        if (!out)
            return false;

        const String & default_str = cache.const_default_value.get<const String &>();
        StringRef default_string_ref{default_str.data(), default_str.size() + 1};
        executeImplStringToStringWithConstDefault(in->getChars(), in->getOffsets(), out->getChars(), out->getOffsets(), default_string_ref);
        return true;
    }

    bool executeStringToStringWithNonConstDefault(const ColumnString * in, IColumn * out_untyped, const IColumn * default_untyped) const
    {
        auto * out = typeid_cast<ColumnString *>(out_untyped);
        if (!out)
            return false;

        const auto * default_col = checkAndGetColumn<ColumnString>(default_untyped);
        if (!default_col)
        {
            throw Exception(ErrorCodes::ILLEGAL_COLUMN, "Illegal column {} of fourth argument of function {}",
                default_untyped->getName(), getName());
        }

        executeImplStringToStringWithNonConstDefault(
            in->getChars(), in->getOffsets(),
            out->getChars(), out->getOffsets(),
            default_col->getChars(), default_col->getOffsets());

        return true;
    }


    template <typename T, typename U>
    void executeImplNumToNumWithConstDefault(const PaddedPODArray<T> & src, PaddedPODArray<U> & dst, U dst_default) const
    {
        const auto & table = *cache.table_num_to_num;
        size_t size = src.size();
        dst.resize(size);
        for (size_t i = 0; i < size; ++i)
        {
            const auto * it = table.find(bit_cast<UInt64>(src[i]));
            if (it)
            {
                if constexpr (std::endian::native == std::endian::little)
                    memcpy(&dst[i], &it->getMapped(), sizeof(dst[i]));
                else
                    memcpy(&dst[i], reinterpret_cast<const char *>(&it->getMapped()) + sizeof(UInt64) - sizeof(dst[i]), sizeof(dst[i]));
            }
            else
                dst[i] = dst_default;
        }
    }

    template <typename T, typename U, typename V>
    void executeImplNumToNumWithNonConstDefault(const PaddedPODArray<T> & src, PaddedPODArray<U> & dst, const PaddedPODArray<V> & dst_default) const
    {
        const auto & table = *cache.table_num_to_num;
        size_t size = src.size();
        dst.resize(size);
        for (size_t i = 0; i < size; ++i)
        {
            const auto * it = table.find(bit_cast<UInt64>(src[i]));
            if (it)
            {
                if constexpr (std::endian::native == std::endian::little)
                    memcpy(&dst[i], &it->getMapped(), sizeof(dst[i]));
                else
                    memcpy(&dst[i], reinterpret_cast<const char *>(&it->getMapped()) + sizeof(UInt64) - sizeof(dst[i]), sizeof(dst[i]));
            }
            else if constexpr (is_decimal<U>)
                dst[i] = static_cast<typename U::NativeType>(dst_default[i]);
            else
                dst[i] = static_cast<U>(dst_default[i]); // NOLINT(bugprone-signed-char-misuse,cert-str34-c)
        }
    }

    template <typename T>
    void executeImplNumToNum(const PaddedPODArray<T> & src, PaddedPODArray<T> & dst) const
    {
        const auto & table = *cache.table_num_to_num;
        size_t size = src.size();
        dst.resize(size);
        for (size_t i = 0; i < size; ++i)
        {
            const auto * it = table.find(bit_cast<UInt64>(src[i]));
            if (it)
            {
                if constexpr (std::endian::native == std::endian::little)
                    memcpy(&dst[i], &it->getMapped(), sizeof(dst[i]));
                else
                    memcpy(&dst[i], reinterpret_cast<const char *>(&it->getMapped()) + sizeof(UInt64) - sizeof(dst[i]), sizeof(dst[i]));
            }
            else
                dst[i] = src[i];
        }
    }

    template <typename T>
    void executeImplNumToStringWithConstDefault(const PaddedPODArray<T> & src,
        ColumnString::Chars & dst_data, ColumnString::Offsets & dst_offsets, StringRef dst_default) const
    {
        const auto & table = *cache.table_num_to_string;
        size_t size = src.size();
        dst_offsets.resize(size);
        ColumnString::Offset current_dst_offset = 0;
        for (size_t i = 0; i < size; ++i)
        {
            const auto * it = table.find(bit_cast<UInt64>(src[i]));
            StringRef ref = it ? it->getMapped() : dst_default;
            dst_data.resize(current_dst_offset + ref.size);
            memcpy(&dst_data[current_dst_offset], ref.data, ref.size);
            current_dst_offset += ref.size;
            dst_offsets[i] = current_dst_offset;
        }
    }

    template <typename T>
    void executeImplNumToStringWithNonConstDefault(const PaddedPODArray<T> & src,
        ColumnString::Chars & dst_data, ColumnString::Offsets & dst_offsets,
        const ColumnString::Chars & dst_default_data, const ColumnString::Offsets & dst_default_offsets) const
    {
        const auto & table = *cache.table_num_to_string;
        size_t size = src.size();
        dst_offsets.resize(size);
        ColumnString::Offset current_dst_offset = 0;
        ColumnString::Offset current_dst_default_offset = 0;
        for (size_t i = 0; i < size; ++i)
        {
            const auto * it = table.find(bit_cast<UInt64>(src[i]));
            StringRef ref;

            if (it)
                ref = it->getMapped();
            else
            {
                ref.data = reinterpret_cast<const char *>(&dst_default_data[current_dst_default_offset]);
                ref.size = dst_default_offsets[i] - current_dst_default_offset;
            }

            dst_data.resize(current_dst_offset + ref.size);
            memcpy(&dst_data[current_dst_offset], ref.data, ref.size);
            current_dst_offset += ref.size;
            current_dst_default_offset = dst_default_offsets[i];
            dst_offsets[i] = current_dst_offset;
        }
    }

    template <typename U>
    void executeImplStringToNumWithConstDefault(
        const ColumnString::Chars & src_data, const ColumnString::Offsets & src_offsets,
        PaddedPODArray<U> & dst, U dst_default) const
    {
        const auto & table = *cache.table_string_to_num;
        size_t size = src_offsets.size();
        dst.resize(size);
        ColumnString::Offset current_src_offset = 0;
        for (size_t i = 0; i < size; ++i)
        {
            StringRef ref{&src_data[current_src_offset], src_offsets[i] - current_src_offset};
            current_src_offset = src_offsets[i];
            const auto * it = table.find(ref);
            if (it)
            {
                if constexpr (std::endian::native == std::endian::little)
                    memcpy(&dst[i], &it->getMapped(), sizeof(dst[i]));
                else
                    memcpy(&dst[i], reinterpret_cast<const char *>(&it->getMapped()) + sizeof(UInt64) - sizeof(dst[i]), sizeof(dst[i]));
            }
            else
                dst[i] = dst_default;
        }
    }

    template <typename U, typename V>
    void executeImplStringToNumWithNonConstDefault(
        const ColumnString::Chars & src_data, const ColumnString::Offsets & src_offsets,
        PaddedPODArray<U> & dst, const PaddedPODArray<V> & dst_default) const
    {
        const auto & table = *cache.table_string_to_num;
        size_t size = src_offsets.size();
        dst.resize(size);
        ColumnString::Offset current_src_offset = 0;
        for (size_t i = 0; i < size; ++i)
        {
            StringRef ref{&src_data[current_src_offset], src_offsets[i] - current_src_offset};
            current_src_offset = src_offsets[i];
            const auto * it = table.find(ref);
            if (it)
            {
                if constexpr (std::endian::native == std::endian::little)
                    memcpy(&dst[i], &it->getMapped(), sizeof(dst[i]));
                else
                    memcpy(&dst[i], reinterpret_cast<const char *>(&it->getMapped()) + sizeof(UInt64) - sizeof(dst[i]), sizeof(dst[i]));
            }
            else if constexpr (is_decimal<U>)
                dst[i] = static_cast<typename U::NativeType>(dst_default[i]);
            else
                dst[i] = static_cast<U>(dst_default[i]); // NOLINT(bugprone-signed-char-misuse,cert-str34-c)
        }
    }

    template <bool with_default>
    void executeImplStringToStringWithOrWithoutConstDefault(
        const ColumnString::Chars & src_data, const ColumnString::Offsets & src_offsets,
        ColumnString::Chars & dst_data, ColumnString::Offsets & dst_offsets, StringRef dst_default) const
    {
        const auto & table = *cache.table_string_to_string;
        size_t size = src_offsets.size();
        dst_offsets.resize(size);
        ColumnString::Offset current_src_offset = 0;
        ColumnString::Offset current_dst_offset = 0;
        for (size_t i = 0; i < size; ++i)
        {
            StringRef src_ref{&src_data[current_src_offset], src_offsets[i] - current_src_offset};
            current_src_offset = src_offsets[i];

            const auto * it = table.find(src_ref);

            StringRef dst_ref = it ? it->getMapped() : (with_default ? dst_default : src_ref);
            dst_data.resize(current_dst_offset + dst_ref.size);
            memcpy(&dst_data[current_dst_offset], dst_ref.data, dst_ref.size);
            current_dst_offset += dst_ref.size;
            dst_offsets[i] = current_dst_offset;
        }
    }

    void executeImplStringToString(
        const ColumnString::Chars & src_data, const ColumnString::Offsets & src_offsets,
        ColumnString::Chars & dst_data, ColumnString::Offsets & dst_offsets) const
    {
        executeImplStringToStringWithOrWithoutConstDefault<false>(src_data, src_offsets, dst_data, dst_offsets, {});
    }

    void executeImplStringToStringWithConstDefault(
        const ColumnString::Chars & src_data, const ColumnString::Offsets & src_offsets,
        ColumnString::Chars & dst_data, ColumnString::Offsets & dst_offsets, StringRef dst_default) const
    {
        executeImplStringToStringWithOrWithoutConstDefault<true>(src_data, src_offsets, dst_data, dst_offsets, dst_default);
    }

    void executeImplStringToStringWithNonConstDefault(
        const ColumnString::Chars & src_data, const ColumnString::Offsets & src_offsets,
        ColumnString::Chars & dst_data, ColumnString::Offsets & dst_offsets,
        const ColumnString::Chars & dst_default_data, const ColumnString::Offsets & dst_default_offsets) const
    {
        const auto & table = *cache.table_string_to_string;
        size_t size = src_offsets.size();
        dst_offsets.resize(size);
        ColumnString::Offset current_src_offset = 0;
        ColumnString::Offset current_dst_offset = 0;
        ColumnString::Offset current_dst_default_offset = 0;
        for (size_t i = 0; i < size; ++i)
        {
            StringRef src_ref{&src_data[current_src_offset], src_offsets[i] - current_src_offset};
            current_src_offset = src_offsets[i];

            const auto * it = table.find(src_ref);
            StringRef dst_ref;

            if (it)
                dst_ref = it->getMapped();
            else
            {
                dst_ref.data = reinterpret_cast<const char *>(&dst_default_data[current_dst_default_offset]);
                dst_ref.size = dst_default_offsets[i] - current_dst_default_offset;
            }

            dst_data.resize(current_dst_offset + dst_ref.size);
            memcpy(&dst_data[current_dst_offset], dst_ref.data, dst_ref.size);
            current_dst_offset += dst_ref.size;
            current_dst_default_offset = dst_default_offsets[i];
            dst_offsets[i] = current_dst_offset;
        }
    }


    /// Different versions of the hash tables to implement the mapping.

    struct Cache
    {
        using NumToNum = HashMap<UInt64, UInt64, HashCRC32<UInt64>>;
        using NumToString = HashMap<UInt64, StringRef, HashCRC32<UInt64>>;     /// Everywhere StringRef's with trailing zero.
        using StringToNum = HashMap<StringRef, UInt64, StringRefHash>;
        using StringToString = HashMap<StringRef, StringRef, StringRefHash>;

        std::unique_ptr<NumToNum> table_num_to_num;
        std::unique_ptr<NumToString> table_num_to_string;
        std::unique_ptr<StringToNum> table_string_to_num;
        std::unique_ptr<StringToString> table_string_to_string;

        Arena string_pool;

        Field const_default_value;    /// Null, if not specified.

        std::atomic<bool> initialized{false};
        std::mutex mutex;
    };

    mutable Cache cache;


    static UInt64 bitCastToUInt64(const Field & x)
    {
        switch (x.getType())
        {
            case Field::Types::UInt64:      return x.get<UInt64>();
            case Field::Types::Int64:       return x.get<Int64>();
            case Field::Types::Float64:     return std::bit_cast<UInt64>(x.get<Float64>());
            case Field::Types::Bool:        return x.get<bool>();
            case Field::Types::Decimal32:   return x.get<DecimalField<Decimal32>>().getValue();
            case Field::Types::Decimal64:   return x.get<DecimalField<Decimal64>>().getValue();
            default:
                throw Exception(ErrorCodes::BAD_ARGUMENTS, "Unexpected type in function 'transform'");
        }
    }

    /// Can be called from different threads. It works only on the first call.
    void initialize(const Array & from, const Array & to, const ColumnsWithTypeAndName & arguments) const
    {
        if (cache.initialized)
            return;

        const size_t size = from.size();
        if (0 == size)
            throw Exception(ErrorCodes::BAD_ARGUMENTS, "Empty arrays are illegal in function {}", getName());

        std::lock_guard lock(cache.mutex);

        if (cache.initialized)
            return;

        if (size != to.size())
            throw Exception(ErrorCodes::BAD_ARGUMENTS, "Second and third arguments of function {} must be arrays of same size", getName());

        Array converted_to;
        const Array * used_to = &to;

        /// Whether the default value is set.

        if (arguments.size() == 4)
        {
            const IColumn * default_col = arguments[3].column.get();
            const ColumnConst * const_default_col = typeid_cast<const ColumnConst *>(default_col);

            if (const_default_col)
                cache.const_default_value = (*const_default_col)[0];

            /// Do we need to convert the elements `to` and `default_value` to the smallest common type that is Float64?
            bool default_col_is_float =
                   checkColumn<ColumnFloat32>(default_col)
                || checkColumn<ColumnFloat64>(default_col)
                || checkColumnConst<ColumnFloat32>(default_col)
                || checkColumnConst<ColumnFloat64>(default_col);

            bool to_is_float = to[0].getType() == Field::Types::Float64;

            if (default_col_is_float && !to_is_float)
            {
                converted_to.resize(size);
                for (size_t i = 0; i < size; ++i)
                    converted_to[i] = applyVisitor(FieldVisitorConvertToNumber<Float64>(), to[i]);
                used_to = &converted_to;
            }
            else if (!default_col_is_float && to_is_float)
            {
                if (const_default_col)
                    cache.const_default_value = applyVisitor(FieldVisitorConvertToNumber<Float64>(), cache.const_default_value);
            }
        }

        /// Note: Doesn't check the duplicates in the `from` array.

        const IDataType & from_type = *arguments[0].type;

        if (from[0].getType() != Field::Types::String)
        {
            if (to[0].getType() != Field::Types::String)
            {
                cache.table_num_to_num = std::make_unique<Cache::NumToNum>();
                auto & table = *cache.table_num_to_num;
                for (size_t i = 0; i < size; ++i)
                {
                    Field key = convertFieldToType(from[i], from_type);
                    if (key.isNull())
                        continue;

                    /// Field may be of Float type, but for the purpose of bitwise equality we can treat them as UInt64
                    table[bitCastToUInt64(key)] = bitCastToUInt64((*used_to)[i]);
                }
            }
            else
            {
                cache.table_num_to_string = std::make_unique<Cache::NumToString>();
                auto & table = *cache.table_num_to_string;
                for (size_t i = 0; i < size; ++i)
                {
                    Field key = convertFieldToType(from[i], from_type);
                    if (key.isNull())
                        continue;

                    const String & str_to = to[i].get<const String &>();
                    StringRef ref{cache.string_pool.insert(str_to.data(), str_to.size() + 1), str_to.size() + 1};
                    table[bitCastToUInt64(key)] = ref;
                }
            }
        }
        else
        {
            if (to[0].getType() != Field::Types::String)
            {
                cache.table_string_to_num = std::make_unique<Cache::StringToNum>();
                auto & table = *cache.table_string_to_num;
                for (size_t i = 0; i < size; ++i)
                {
                    const String & str_from = from[i].get<const String &>();
                    StringRef ref{cache.string_pool.insert(str_from.data(), str_from.size() + 1), str_from.size() + 1};
                    table[ref] = bitCastToUInt64((*used_to)[i]);
                }
            }
            else
            {
                cache.table_string_to_string = std::make_unique<Cache::StringToString>();
                auto & table = *cache.table_string_to_string;
                for (size_t i = 0; i < size; ++i)
                {
                    const String & str_from = from[i].get<const String &>();
                    const String & str_to = to[i].get<const String &>();
                    StringRef ref_from{cache.string_pool.insert(str_from.data(), str_from.size() + 1), str_from.size() + 1};
                    StringRef ref_to{cache.string_pool.insert(str_to.data(), str_to.size() + 1), str_to.size() + 1};
                    table[ref_from] = ref_to;
                }
            }
        }

        cache.initialized = true;
    }
};

}

REGISTER_FUNCTION(Transform)
{
    factory.registerFunction<FunctionTransform>();
}

}
