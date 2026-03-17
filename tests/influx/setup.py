"""InfluxDB setup — creates database and retention policy."""
import logging
import aiohttp

logger = logging.getLogger(__name__)


async def ensure_database(url: str, database: str):
    """Create the InfluxDB database if it doesn't exist. Set 30-day retention."""
    url = url.rstrip('/')
    query_url = f"{url}/query"

    async with aiohttp.ClientSession() as session:
        # Create database
        async with session.post(query_url, data={
            "q": f"CREATE DATABASE {database}"
        }) as resp:
            if resp.status == 200:
                logger.info(f"Ensured InfluxDB database '{database}' exists")
            else:
                text = await resp.text()
                logger.error(f"Failed to create database: {text}")

        # Set retention policy (30 days)
        rp_query = (
            f'CREATE RETENTION POLICY "thirty_days" ON "{database}" '
            f'DURATION 30d REPLICATION 1 DEFAULT'
        )
        try:
            async with session.post(query_url, data={"q": rp_query}) as resp:
                if resp.status == 200:
                    logger.info("Set 30-day retention policy")
                else:
                    # May already exist — try ALTER instead
                    alter_query = rp_query.replace("CREATE", "ALTER")
                    await session.post(query_url, data={"q": alter_query})
        except Exception as e:
            logger.warning(f"Retention policy setup: {e}")
