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


#include <memory>
#include <string>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/client/read_preference.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/commands.h"
#include "mongo/db/database_name.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/s/client/shard.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/grid.h"
#include "mongo/s/request_types/move_range_request_gen.h"
#include "mongo/util/assert_util.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand


namespace mongo {
namespace {

class ClusterMoveRangeCommand final : public TypedCommand<ClusterMoveRangeCommand> {
public:
    using Request = ClusterMoveRange;

    std::string help() const override {
        return "Example: move range starting from {num : 7} to shard001 (max bound automatically "
               "chosen)\n  { moveRange : 'test.foo' , min : { num : 7 } , to : 'shard0001' }\n"
               "Example: move range with lower bound 0 and upper bound 10 to shard001\n"
               "  { moveRange : 'test.foo' , min : { num : 0 } , max: { num : 10 } "
               " , to : 'shard001' }\n";
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }

    bool adminOnly() const override {
        return true;
    }

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        void typedRun(OperationContext* opCtx) {
            const auto nss = ns();
            const auto& req = request();

            uassert(ErrorCodes::InvalidOptions,
                    "Missing required parameter 'min' or 'max'",
                    req.getMin() || req.getMax());

            ConfigsvrMoveRange configsvrRequest(nss);
            configsvrRequest.setDbName(DatabaseName::kAdmin);
            configsvrRequest.setMoveRangeRequestBase(req.getMoveRangeRequestBase());
            configsvrRequest.setForceJumbo(request().getForceJumbo() ? ForceJumbo::kForceManual
                                                                     : ForceJumbo::kDoNotForce);

            auto configShard = Grid::get(opCtx)->shardRegistry()->getConfigShard();
            const auto commandResponse = uassertStatusOK(configShard->runCommand(
                opCtx,
                ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                DatabaseName::kAdmin.toString(),
                configsvrRequest.toBSON(BSON(WriteConcernOptions::kWriteConcernField
                                             << opCtx->getWriteConcern().toBSON())),
                Shard::RetryPolicy::kIdempotent));

            uassertStatusOK(Shard::CommandResponse::getEffectiveStatus(commandResponse));
        }

    private:
        NamespaceString ns() const override {
            return request().getCommandParameter();
        }

        bool supportsWriteConcern() const override {
            return true;
        }

        void doCheckAuthorization(OperationContext* opCtx) const override {
            uassert(ErrorCodes::Unauthorized,
                    "Unauthorized",
                    AuthorizationSession::get(opCtx->getClient())
                        ->isAuthorizedForActionsOnResource(ResourcePattern::forExactNamespace(ns()),
                                                           ActionType::moveChunk));
        }
    };
} moveRange;

}  // namespace
}  // namespace mongo
