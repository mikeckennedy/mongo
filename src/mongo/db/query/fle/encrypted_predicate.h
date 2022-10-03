/**
 *    Copyright (C) 2022-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include <functional>

#include "mongo/base/init.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/crypto/fle_crypto.h"
#include "mongo/crypto/fle_field_schema_gen.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/matcher/expression_leaf.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/query/fle/query_rewriter_interface.h"

/**
 * This file contains an abstract class that describes rewrites on agg Expressions and
 * MatchExpressions for individual encrypted index types. Subclasses of this class represent
 * concrete encrypted index types, like Equality and Range.
 *
 * This class is not responsible for traversing expression trees, but instead takes leaf
 * expressions that it may replace. Tree traversal is handled by the QueryRewriter.
 */

namespace mongo {
namespace fle {

// Virtual functions can't be templated, so in order to write a function which can take in either a
// BSONElement or a Value&, we need to create a variant type to use in function signatures.
// std::reference_wrapper is necessary to avoid copying the Value because references alone cannot be
// included in a variant. BSONElement can be passed by value because it is just a pointer into an
// owning BSONObj.
using BSONValue = stdx::variant<BSONElement, std::reference_wrapper<Value>>;

/**
 * Parse a find payload from either a BSONElement or a Value. All ParsedFindPayload types should
 * have constructors for both BSONElements and Values, which will enable this function to work on
 * both types.
 */
template <typename T>
T parseFindPayload(BSONValue payload) {
    return stdx::visit(OverloadedVisitor{[&](BSONElement payload) { return T(payload); },
                                         [&](Value payload) { return T(payload); }},
                       payload);
}

std::unique_ptr<Expression> makeTagDisjunction(ExpressionContext* expCtx,
                                               std::vector<Value>&& tags);

/**
 * Convert a vector of PrfBlocks to a BSONArray for use in MatchExpression tag generation.
 */
BSONArray toBSONArray(std::vector<PrfBlock>&& vec);

/**
 * Convert a vector of PrfBlocks to a vector of Values for use in Agg tag generation.
 */
std::vector<Value> toValues(std::vector<PrfBlock>&& vec);

std::unique_ptr<MatchExpression> makeTagDisjunction(BSONArray&& tagArray);

void logTagsExceeded(const ExceptionFor<ErrorCodes::FLEMaxTagLimitExceeded>& ex);
/**
 * Interface for implementing a server rewrite for an encrypted index. Each type of predicate
 * should have its own subclass that implements the virtual methods in this class.
 */
class EncryptedPredicate {
public:
    EncryptedPredicate(const QueryRewriterInterface* rewriter) : _rewriter(rewriter) {}

    /**
     * Rewrite a terminal expression for this encrypted predicate. If this function returns
     * nullptr, then no rewrite needs to be done. Rewrites generally transform predicates from one
     * kind of expression to another, either a $in or an $_internalFle* runtime expression, and so
     * this function will allocate a new expression and return a unique_ptr to it.
     */
    template <typename T>
    std::unique_ptr<T> rewrite(T* expr) const {
        auto mode = _rewriter->getEncryptedCollScanMode();
        if (mode != EncryptedCollScanMode::kForceAlways) {
            try {
                return rewriteToTagDisjunction(expr);
            } catch (const ExceptionFor<ErrorCodes::FLEMaxTagLimitExceeded>& ex) {
                // LOGV2 can't be called from a header file, so this call is factored out to a
                // function defined in the cpp file.
                logTagsExceeded(ex);
                if (mode != EncryptedCollScanMode::kUseIfNeeded) {
                    throw;
                }
            }
        }
        return rewriteToRuntimeComparison(expr);
    }

protected:
    /**
     * Check if the passed-in payload is a FLE2 find payload for the right encrypted index type.
     */
    virtual bool isPayload(const BSONElement& elt) const {
        if (!elt.isBinData(BinDataType::Encrypt)) {
            return false;
        }
        int dataLen;
        auto data = elt.binData(dataLen);

        // Check that the BinData's subtype is 6, and its sub-subtype is equal to this predicate's
        // encryptedBinDataType.
        return dataLen >= 1 &&
            static_cast<uint8_t>(data[0]) == static_cast<uint8_t>(encryptedBinDataType());
    }

    /**
     * Check if the passed-in payload is a FLE2 find payload for the right encrypted index type.
     */
    virtual bool isPayload(const Value& v) const {
        if (v.getType() != BSONType::BinData) {
            return false;
        }

        auto binData = v.getBinData();
        // Check that the BinData's subtype is 6, and its sub-subtype is equal to this predicate's
        // encryptedBinDataType.
        return binData.type == BinDataType::Encrypt && binData.length >= 1 &&
            static_cast<uint8_t>(encryptedBinDataType()) ==
            static_cast<const uint8_t*>(binData.data)[0];
    }
    /**
     * Generate tags from a FLE2 Find Payload. This function takes in a variant of BSONElement and
     * Value so that it can be used in both the MatchExpression and Aggregation contexts. Virtual
     * functions can't also be templated, which is why we need the runtime dispatch on the variant.
     */
    virtual std::vector<PrfBlock> generateTags(BSONValue payload) const = 0;

    /**
     * Rewrite to a tag disjunction on the __safeContent__ field.
     */
    virtual std::unique_ptr<MatchExpression> rewriteToTagDisjunction(
        MatchExpression* expr) const = 0;
    /**
     * Rewrite to a tag disjunction on the __safeContent__ field.
     */
    virtual std::unique_ptr<Expression> rewriteToTagDisjunction(Expression* expr) const = 0;

    /**
     * Rewrite to an expression which can generate tags at runtime during an encrypted collscan.
     */
    virtual std::unique_ptr<MatchExpression> rewriteToRuntimeComparison(
        MatchExpression* expr) const = 0;
    /**
     * Rewrite to an expression which can generate tags at runtime during an encrypted collscan.
     */
    virtual std::unique_ptr<Expression> rewriteToRuntimeComparison(Expression* expr) const = 0;

    const QueryRewriterInterface* _rewriter;

private:
    /**
     * Sub-subtype associated with the find payload for this encrypted predicate.
     */
    virtual EncryptedBinDataType encryptedBinDataType() const = 0;
};

/**
 * Encrypted predicate rewrites are registered at startup time using MONGO_INITIALIZER blocks.
 * MatchExpression rewrites are keyed on the MatchExpressionType enum, and Agg Expression rewrites
 * are keyed on the dynamic type for the Expression subclass.
 */

using ExpressionToRewriteMap = stdx::unordered_map<
    std::type_index,
    std::function<std::unique_ptr<Expression>(QueryRewriterInterface*, Expression*)>>;

extern ExpressionToRewriteMap aggPredicateRewriteMap;

using MatchTypeToRewriteMap = stdx::unordered_map<
    MatchExpression::MatchType,
    std::function<std::unique_ptr<MatchExpression>(QueryRewriterInterface*, MatchExpression*)>>;

extern MatchTypeToRewriteMap matchPredicateRewriteMap;

/**
 * Register an agg rewrite if a condition is true at startup time.
 */
#define REGISTER_ENCRYPTED_AGG_PREDICATE_REWRITE_GUARDED(className, rewriteClass, isEnabledExpr)   \
    MONGO_INITIALIZER(encryptedAggPredicateRewriteFor_##className)(InitializerContext*) {          \
                                                                                                   \
        invariant(aggPredicateRewriteMap.find(typeid(className)) == aggPredicateRewriteMap.end()); \
        aggPredicateRewriteMap[typeid(className)] = [&](auto* rewriter, auto* expr) {              \
            if (isEnabledExpr) {                                                                   \
                return rewriteClass{rewriter}.rewrite(expr);                                       \
            } else {                                                                               \
                return std::unique_ptr<Expression>(nullptr);                                       \
            }                                                                                      \
        };                                                                                         \
    }

/**
 * Register an agg rewrite unconditionally.
 */
#define REGISTER_ENCRYPTED_AGG_PREDICATE_REWRITE(matchType, rewriteClass) \
    REGISTER_ENCRYPTED_AGG_PREDICATE_REWRITE_GUARDED(matchType, rewriteClass, true)

/**
 * Register an agg rewrite behind a feature flag.
 */
#define REGISTER_ENCRYPTED_AGG_PREDICATE_REWRITE_WITH_FLAG(matchType, rewriteClass, featureFlag) \
    REGISTER_ENCRYPTED_AGG_PREDICATE_REWRITE_GUARDED(                                            \
        matchType, rewriteClass, featureFlag.isEnabled(serverGlobalParams.featureCompatibility))

/**
 * Register a MatchExpression rewrite if a condition is true at startup time.
 */
#define REGISTER_ENCRYPTED_MATCH_PREDICATE_REWRITE_GUARDED(matchType, rewriteClass, isEnabledExpr) \
    MONGO_INITIALIZER(encryptedMatchPredicateRewriteFor_##matchType)(InitializerContext*) {        \
                                                                                                   \
        invariant(matchPredicateRewriteMap.find(MatchExpression::matchType) ==                     \
                  matchPredicateRewriteMap.end());                                                 \
        matchPredicateRewriteMap[MatchExpression::matchType] = [&](auto* rewriter, auto* expr) {   \
            if (isEnabledExpr) {                                                                   \
                return rewriteClass{rewriter}.rewrite(expr);                                       \
            } else {                                                                               \
                return std::unique_ptr<MatchExpression>(nullptr);                                  \
            }                                                                                      \
        };                                                                                         \
    };
/**
 * Register a MatchExpression rewrite unconditionally.
 */
#define REGISTER_ENCRYPTED_MATCH_PREDICATE_REWRITE(matchType, rewriteClass) \
    REGISTER_ENCRYPTED_MATCH_PREDICATE_REWRITE_GUARDED(matchType, rewriteClass, true)

/**
 * Register a MatchExpression rewrite behind a feature flag.
 */
#define REGISTER_ENCRYPTED_MATCH_PREDICATE_REWRITE_WITH_FLAG(matchType, rewriteClass, featureFlag) \
    REGISTER_ENCRYPTED_MATCH_PREDICATE_REWRITE_GUARDED(                                            \
        matchType, rewriteClass, featureFlag.isEnabled(serverGlobalParams.featureCompatibility))
}  // namespace fle
}  // namespace mongo
