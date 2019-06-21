// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use crate::constants::{AUTHORIZE_URI, DEFAULT_SCOPES, FUCHSIA_CLIENT_ID, REDIRECT_URI};
use crate::error::{AuthProviderError, ResultExt};
use crate::web::StandaloneWebFrame;
use failure::{format_err, Error};
use fidl;
use fidl::encoding::OutOfLine;
use fidl::endpoints::ClientEnd;
use fidl_fuchsia_auth::{
    AssertionJwtParams, AttestationJwtParams, AttestationSignerMarker, AuthChallenge,
    AuthProviderGetAppAccessTokenFromAssertionJwtResponder, AuthProviderGetAppAccessTokenResponder,
    AuthProviderGetAppFirebaseTokenResponder, AuthProviderGetAppIdTokenResponder,
    AuthProviderGetPersistentCredentialFromAttestationJwtResponder,
    AuthProviderGetPersistentCredentialResponder, AuthProviderRequest, AuthProviderRequestStream,
    AuthProviderRevokeAppOrPersistentCredentialResponder, AuthProviderStatus, AuthToken,
    AuthenticationUiContextMarker, FirebaseToken, UserProfileInfo,
};
use fuchsia_scenic::ViewTokenPair;
use futures::prelude::*;
use log::{info, warn};
use std::borrow::Cow;
use std::collections::HashMap;
use url::Url;

type AuthProviderResult<T> = Result<T, AuthProviderError>;
#[derive(Debug, PartialEq)]
struct AuthCode(String);
struct RefreshToken(String);
struct AccessToken(String);

/// Trait for structs capable of creating new Web frames.
pub trait WebFrameSupplier {
    /// Creates a new `StandaloneWebFrame`.  This method guarantees that the
    /// new frame is in its own web context.
    /// Although implementation of this method does not require state, `self`
    /// is added here to allow injection of mocks with canned responses.
    fn new_standalone_frame(&self) -> Result<StandaloneWebFrame, Error>;
}

/// An implementation of the `AuthProvider` FIDL protocol that communicates
/// with the Google identity system to perform authentication for and issue
/// tokens for Google accounts.
pub struct GoogleAuthProvider<W: WebFrameSupplier> {
    /// A supplier used to generate web frames on demand.
    web_frame_supplier: W,
}

impl<W: WebFrameSupplier> GoogleAuthProvider<W> {
    /// Create a new GoogleAuthProvider.
    pub fn new(web_frame_supplier: W) -> Self {
        GoogleAuthProvider { web_frame_supplier }
    }

    /// Handle requests passed to the supplied stream.
    pub async fn handle_requests_from_stream(
        &self,
        mut stream: AuthProviderRequestStream,
    ) -> Result<(), Error> {
        while let Some(request) = await!(stream.try_next())? {
            await!(self.handle_request(request));
        }
        Ok(())
    }

    /// Handle a single `AuthProviderRequest`.
    async fn handle_request(&self, req: AuthProviderRequest) {
        match req {
            AuthProviderRequest::GetPersistentCredential {
                auth_ui_context,
                user_profile_id,
                responder,
            } => responder.send_result(await!(
                self.get_persistent_credential(auth_ui_context, user_profile_id)
            )),
            AuthProviderRequest::GetAppAccessToken { credential, client_id, scopes, responder } => {
                responder
                    .send_result(await!(self.get_app_access_token(credential, client_id, scopes)))
            }
            AuthProviderRequest::GetAppIdToken { credential, audience, responder } => {
                responder.send_result(await!(self.get_app_id_token(credential, audience)))
            }
            AuthProviderRequest::GetAppFirebaseToken { id_token, firebase_api_key, responder } => {
                responder
                    .send_result(await!(self.get_app_firebase_token(id_token, firebase_api_key)))
            }
            AuthProviderRequest::RevokeAppOrPersistentCredential { credential, responder } => {
                responder.send_result(await!(self.revoke_app_or_persistent_credential(credential)))
            }
            AuthProviderRequest::GetPersistentCredentialFromAttestationJwt {
                attestation_signer,
                jwt_params,
                auth_ui_context,
                user_profile_id,
                responder,
            } => responder.send_result(await!(self
                .get_persistent_credential_from_attestation_jwt(
                    attestation_signer,
                    jwt_params,
                    auth_ui_context,
                    user_profile_id
                ))),
            AuthProviderRequest::GetAppAccessTokenFromAssertionJwt {
                attestation_signer,
                jwt_params,
                credential,
                scopes,
                responder,
            } => responder.send_result(await!(self.get_app_access_token_from_assertion_jwt(
                attestation_signer,
                jwt_params,
                credential,
                scopes
            ))),
        }
    }

    /// Implementation of `GetPersistentCredential` method for the
    /// `AuthProvider` interface.
    async fn get_persistent_credential(
        &self,
        auth_ui_context: Option<ClientEnd<AuthenticationUiContextMarker>>,
        user_profile_id: Option<String>,
    ) -> AuthProviderResult<(RefreshToken, UserProfileInfo)> {
        match auth_ui_context {
            Some(ui_context) => {
                let auth_code = await!(self.get_auth_code(ui_context, user_profile_id))?;
                info!("Received auth code of length: {:?}", &auth_code.0.len());
                // TODO(satsukiu): Complete auth code exchange and user profile info retrieval
                let (_refresh_token, access_token) = await!(self.exchange_auth_code(auth_code))?;
                let _user_profile_info = await!(self.get_user_profile_info(access_token))?;
                Err(AuthProviderError::new(AuthProviderStatus::InternalError))
            }
            None => Err(AuthProviderError::new(AuthProviderStatus::BadRequest)),
        }
    }

    /// Implementation of `GetAppAccessToken` method for the `AuthProvider`
    /// interface.
    async fn get_app_access_token(
        &self,
        _credential: String,
        _client_id: Option<String>,
        _scopes: Vec<String>,
    ) -> AuthProviderResult<AuthToken> {
        // TODO(satsukiu): implement
        Err(AuthProviderError::new(AuthProviderStatus::InternalError))
    }

    /// Implementation of `GetAppIdToken` method for the `AuthProvider`
    /// interface.
    async fn get_app_id_token(
        &self,
        _credential: String,
        _audience: Option<String>,
    ) -> AuthProviderResult<AuthToken> {
        // TODO(satsukiu): implement
        Err(AuthProviderError::new(AuthProviderStatus::InternalError))
    }

    /// Implementation of `GetAppFirebaseToken` method for the `AuthProvider`
    /// interface.
    async fn get_app_firebase_token(
        &self,
        _id_token: String,
        _firebase_api_key: String,
    ) -> AuthProviderResult<FirebaseToken> {
        // TODO(satsukiu): implement
        Err(AuthProviderError::new(AuthProviderStatus::InternalError))
    }

    /// Implementation of `RevokeAppOrPersistentCredential` method for the
    /// `AuthProvider` interface.
    async fn revoke_app_or_persistent_credential(
        &self,
        _credential: String,
    ) -> AuthProviderResult<()> {
        // TODO(satsukiu): implement
        Err(AuthProviderError::new(AuthProviderStatus::InternalError))
    }

    /// Implementation of `GetPersistentCredentialFromAttestationJWT` method
    /// for the `AuthProvider` interface.
    async fn get_persistent_credential_from_attestation_jwt(
        &self,
        _attestation_signer: ClientEnd<AttestationSignerMarker>,
        _jwt_params: AttestationJwtParams,
        _auth_ui_context: Option<ClientEnd<AuthenticationUiContextMarker>>,
        _user_profile_id: Option<String>,
    ) -> AuthProviderResult<(String, AuthToken, AuthChallenge, UserProfileInfo)> {
        // Remote attestation flow is not supported for traditional OAuth.
        Err(AuthProviderError::new(AuthProviderStatus::InternalError))
    }

    /// Implementation of `GetAppAccessTokenFromAssertionJWT` method for the
    /// `AuthProvider` interface.
    async fn get_app_access_token_from_assertion_jwt(
        &self,
        _attestation_signer: ClientEnd<AttestationSignerMarker>,
        _jwt_params: AssertionJwtParams,
        _credential: String,
        _scopes: Vec<String>,
    ) -> AuthProviderResult<(String, AuthToken, AuthChallenge)> {
        // Remote attestation flow is not supported for traditional OAuth.
        Err(AuthProviderError::new(AuthProviderStatus::InternalError))
    }

    /// Direct user through Google OAuth authorization flow and obtain an auth
    /// code.
    async fn get_auth_code(
        &self,
        auth_ui_context: ClientEnd<AuthenticationUiContextMarker>,
        user_profile_id: Option<String>,
    ) -> AuthProviderResult<AuthCode> {
        let auth_ui_context =
            auth_ui_context.into_proxy().auth_provider_status(AuthProviderStatus::UnknownError)?;
        let mut web_frame = self
            .web_frame_supplier
            .new_standalone_frame()
            .auth_provider_status(AuthProviderStatus::UnknownError)?;
        let ViewTokenPair { view_token, mut view_holder_token } =
            ViewTokenPair::new().auth_provider_status(AuthProviderStatus::UnknownError)?;
        let authorize_url = Self::authorize_url(user_profile_id)?;

        await!(web_frame.display_url(view_token, authorize_url))?;
        auth_ui_context
            .start_overlay(&mut view_holder_token)
            .auth_provider_status(AuthProviderStatus::UnknownError)?;

        let redirect_url = await!(web_frame.wait_for_redirect(REDIRECT_URI.clone()))?;

        if let Err(err) = auth_ui_context.stop_overlay() {
            warn!("Error while attempting to stop UI overlay: {:?}", err);
        }

        Self::parse_auth_code_from_redirect(redirect_url)
    }

    fn authorize_url(user_profile_id: Option<String>) -> AuthProviderResult<Url> {
        let mut params = vec![
            ("scope", DEFAULT_SCOPES.as_str()),
            ("glif", "false"), // TODO(satsukiu): add a command line parameter to set this
            ("response_type", "code"),
            ("redirect_uri", REDIRECT_URI.as_str()),
            ("client_id", FUCHSIA_CLIENT_ID),
        ]; // TODO(satsukiu): add 'state' parameter here and verify it in the redirect.
        if let Some(user) = user_profile_id.as_ref() {
            params.push(("login_hint", user));
        }

        Url::parse_with_params(AUTHORIZE_URI.as_str(), &params)
            .auth_provider_status(AuthProviderStatus::InternalError)
    }

    fn parse_auth_code_from_redirect(url: Url) -> AuthProviderResult<AuthCode> {
        if (url.scheme(), url.domain(), url.path())
            != (REDIRECT_URI.scheme(), REDIRECT_URI.domain(), REDIRECT_URI.path())
        {
            return Err(AuthProviderError::new(AuthProviderStatus::InternalError)
                .with_cause(format_err!("Redirected to unexpected URL")));
        }

        let params = url.query_pairs().collect::<HashMap<Cow<str>, Cow<str>>>();

        if let Some(auth_code) = params.get("code") {
            Ok(AuthCode(String::from(auth_code.as_ref())))
        } else if let Some(error_code) = params.get("error") {
            let error_status = match error_code.as_ref() {
                "access_denied" => AuthProviderStatus::UserCancelled,
                "server_error" => AuthProviderStatus::OauthServerError,
                "temporarily_unavailable" => AuthProviderStatus::OauthServerError,
                _ => AuthProviderStatus::UnknownError,
            };
            Err(AuthProviderError::new(error_status))
        } else {
            Err(AuthProviderError::new(AuthProviderStatus::UnknownError)
                .with_cause(format_err!("Authorize redirect contained neither code nor error")))
        }
    }

    /// Trades an OAuth auth code for a refresh token and access token.
    async fn exchange_auth_code(
        &self,
        _auth_code: AuthCode,
    ) -> AuthProviderResult<(RefreshToken, AccessToken)> {
        // TODO(satsukiu): implement
        Err(AuthProviderError::new(AuthProviderStatus::InternalError))
    }

    /// Use an access token to retrieve profile information.
    async fn get_user_profile_info(
        &self,
        _access_token: AccessToken,
    ) -> AuthProviderResult<UserProfileInfo> {
        // TODO(satukiu): implement
        Err(AuthProviderError::new(AuthProviderStatus::InternalError))
    }
}

/// Trait containing logic for sending responses.  Enables `Result` returntype
/// for API implementations.
trait Responder: Sized {
    type Data;
    /// Consume the responder to send a result.  Logs any errors encountered
    /// while sending response.
    fn send_result(self, result: AuthProviderResult<Self::Data>) {
        let send_result = match result {
            Ok(val) => self.send_raw(AuthProviderStatus::Ok, Some(val)),
            Err(err) => self.send_raw(err.status, None),
        };
        if let Err(err) = send_result {
            warn!("Error sending response to {}: {:?}", Self::METHOD_NAME, err);
        }
    }

    /// Send response without handling failures.
    fn send_raw(
        self,
        status: AuthProviderStatus,
        data: Option<Self::Data>,
    ) -> Result<(), fidl::Error>;

    const METHOD_NAME: &'static str;
}

impl Responder for AuthProviderGetPersistentCredentialResponder {
    type Data = (RefreshToken, UserProfileInfo);
    const METHOD_NAME: &'static str = "GetPersistentCredential";

    fn send_raw(
        self,
        status: AuthProviderStatus,
        data: Option<(RefreshToken, UserProfileInfo)>,
    ) -> Result<(), fidl::Error> {
        match data {
            None => self.send(status, None, None),
            Some((refresh_token, mut user_profile_info)) => self.send(
                status,
                Some(refresh_token.0.as_str()),
                Some(OutOfLine(&mut user_profile_info)),
            ),
        }
    }
}

impl Responder for AuthProviderGetAppAccessTokenResponder {
    type Data = AuthToken;
    const METHOD_NAME: &'static str = "GetAppAccessToken";

    fn send_raw(
        self,
        status: AuthProviderStatus,
        mut data: Option<AuthToken>,
    ) -> Result<(), fidl::Error> {
        self.send(status, data.as_mut().map(OutOfLine))
    }
}

impl Responder for AuthProviderGetAppIdTokenResponder {
    type Data = AuthToken;
    const METHOD_NAME: &'static str = "GetAppIdToken";

    fn send_raw(
        self,
        status: AuthProviderStatus,
        mut data: Option<AuthToken>,
    ) -> Result<(), fidl::Error> {
        self.send(status, data.as_mut().map(OutOfLine))
    }
}

impl Responder for AuthProviderGetAppFirebaseTokenResponder {
    type Data = FirebaseToken;
    const METHOD_NAME: &'static str = "GetAppFirebaseToken";

    fn send_raw(
        self,
        status: AuthProviderStatus,
        mut data: Option<FirebaseToken>,
    ) -> Result<(), fidl::Error> {
        self.send(status, data.as_mut().map(OutOfLine))
    }
}

impl Responder for AuthProviderRevokeAppOrPersistentCredentialResponder {
    type Data = ();
    const METHOD_NAME: &'static str = "RevokeAppOrPersistentCredential";

    fn send_raw(self, status: AuthProviderStatus, _data: Option<()>) -> Result<(), fidl::Error> {
        self.send(status)
    }
}

impl Responder for AuthProviderGetPersistentCredentialFromAttestationJwtResponder {
    type Data = (String, AuthToken, AuthChallenge, UserProfileInfo);
    const METHOD_NAME: &'static str = "GetPersistentCredentialFromAttestationJwt";

    fn send_raw(
        self,
        status: AuthProviderStatus,
        data: Option<(String, AuthToken, AuthChallenge, UserProfileInfo)>,
    ) -> Result<(), fidl::Error> {
        match data {
            None => self.send(status, None, None, None, None),
            Some((credential, mut auth_token, mut auth_challenge, mut user_profile_info)) => self
                .send(
                    status,
                    Some(credential.as_str()),
                    Some(OutOfLine(&mut auth_token)),
                    Some(OutOfLine(&mut auth_challenge)),
                    Some(OutOfLine(&mut user_profile_info)),
                ),
        }
    }
}

impl Responder for AuthProviderGetAppAccessTokenFromAssertionJwtResponder {
    type Data = (String, AuthToken, AuthChallenge);
    const METHOD_NAME: &'static str = "GetAppAccessTokenFromAssertionJwt";

    fn send_raw(
        self,
        status: AuthProviderStatus,
        data: Option<(String, AuthToken, AuthChallenge)>,
    ) -> Result<(), fidl::Error> {
        match data {
            None => self.send(status, None, None, None),
            Some((credential, mut auth_token, mut auth_challenge)) => self.send(
                status,
                Some(credential.as_str()),
                Some(OutOfLine(&mut auth_token)),
                Some(OutOfLine(&mut auth_challenge)),
            ),
        }
    }
}

#[cfg(test)]
mod tests {

    use super::*;
    use fidl::endpoints::{create_proxy, create_request_stream, ServerEnd};
    use fidl_fuchsia_auth::{AuthProviderMarker, AuthProviderProxy};
    use fidl_fuchsia_web::{ContextMarker, FrameMarker};
    use fuchsia_async as fasync;
    use fuchsia_zircon as zx;

    /// A no-op implementation of `WebFrameSupplier`
    struct TestWebFrameSupplier;

    impl WebFrameSupplier for TestWebFrameSupplier {
        fn new_standalone_frame(&self) -> Result<StandaloneWebFrame, Error> {
            let (context, _) = create_proxy::<ContextMarker>()?;

            let (frame, _) = create_request_stream::<FrameMarker>()?;
            Ok(StandaloneWebFrame::new(context, frame.into_proxy()?))
        }
    }

    fn url_with_query(url_base: &Url, query: &str) -> Url {
        let mut url = url_base.clone();
        url.set_query(Some(query));
        url
    }

    /// Creates an auth provider.
    fn get_auth_provider_proxy() -> AuthProviderProxy {
        let (server_chan, client_chan) = zx::Channel::create().expect("Failed to create channel");

        let server_end = ServerEnd::<AuthProviderMarker>::new(server_chan);
        let request_stream = server_end.into_stream().expect("Failed to create request stream");
        let frame_supplier = TestWebFrameSupplier {};
        let auth_provider = GoogleAuthProvider::<TestWebFrameSupplier>::new(frame_supplier);
        fasync::spawn(async move {
            await!(auth_provider.handle_requests_from_stream(request_stream))
                .expect("Error handling AuthProvider channel");
        });

        let client_chan =
            fasync::Channel::from_channel(client_chan).expect("Channel client creation failed.");
        AuthProviderProxy::new(client_chan)
    }

    #[fasync::run_until_stalled(test)]
    async fn test_auth_code_from_redirect() -> Result<(), Error> {
        let success_url = url_with_query(&REDIRECT_URI, "code=test-auth-code");
        assert_eq!(
            AuthCode(String::from("test-auth-code")),
            GoogleAuthProvider::<TestWebFrameSupplier>::parse_auth_code_from_redirect(success_url)
                .unwrap()
        );

        let canceled_url = url_with_query(&REDIRECT_URI, "error=access_denied");
        assert_eq!(
            AuthProviderStatus::UserCancelled,
            GoogleAuthProvider::<TestWebFrameSupplier>::parse_auth_code_from_redirect(canceled_url)
                .unwrap_err()
                .status
        );
        Ok(())
    }

    #[fasync::run_until_stalled(test)]
    async fn test_get_persistent_credential_requires_ui_context() -> Result<(), Error> {
        let auth_provider = get_auth_provider_proxy();
        let result = await!(auth_provider.get_persistent_credential(None, None))?;
        assert_eq!(result.0, AuthProviderStatus::BadRequest);
        Ok(())
    }

    #[fasync::run_until_stalled(test)]
    async fn test_get_app_access_token() -> Result<(), Error> {
        let auth_provider = get_auth_provider_proxy();
        let result = await!(auth_provider.get_app_access_token(
            "credential",
            None,
            &mut vec![].into_iter()
        ))?;
        assert_eq!(result.0, AuthProviderStatus::InternalError);
        Ok(())
    }

    #[fasync::run_until_stalled(test)]
    async fn test_get_app_id_token() -> Result<(), Error> {
        let auth_provider = get_auth_provider_proxy();
        let result = await!(auth_provider.get_app_id_token("credential", None))?;
        assert_eq!(result.0, AuthProviderStatus::InternalError);
        Ok(())
    }

    #[fasync::run_until_stalled(test)]
    async fn test_get_app_firebase_token() -> Result<(), Error> {
        let auth_provider = get_auth_provider_proxy();
        let result = await!(auth_provider.get_app_firebase_token("id_token", "api_key"))?;
        assert_eq!(result.0, AuthProviderStatus::InternalError);
        Ok(())
    }

    #[fasync::run_until_stalled(test)]
    async fn test_revoke_app_or_persistent_credential() -> Result<(), Error> {
        let auth_provider = get_auth_provider_proxy();
        let result = await!(auth_provider.revoke_app_or_persistent_credential("credential"))?;
        assert_eq!(result, AuthProviderStatus::InternalError);
        Ok(())
    }
}
